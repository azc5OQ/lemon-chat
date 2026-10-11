// keys.js is embedded in template.html along with the other client files, and in the node bundle
// it is the key handling: the rsa keypair size rules, the diffie-hellman session keys, the aes message
// bodies and the metadata envelope of every frame, and the channel keys a maintainer hands out (with
// the wait timer that votes a silent maintainer out)
// workers.js does the same wire work inside the worker; messages.js calls the channel key side

// state private to this file
// the maintainer reset: whenever this client waits for keys, a timer is armed; keys that do not arrive
// in time send a reset_channel_maintainer vote (re-sent every timeout), and once more than half of the
// channel has voted the server deposes the maintainer and announces a new one
var MAINTAINER_KEYS_WAIT_TIMEOUT_MS = 5000;

var maintainer_keys_wait_timer = null;

var maintainer_keys_wait_channel_id = -1;

// ---- identity and wire crypto ----

/**
 * @brief the smallest key size we can create that satisfies the server
 *        a modulus may come out one bit short, so a server asking for exactly N is satisfied by our
 *        N; anything between the offered sizes rounds up to the next one we can actually generate
 *
 * @param number minimum_bits -> the size the server demands
 *
 * @return number the size to generate, 0 when no offered size is big enough
 */
function keys__pick_rsa_key_bits_for_minimum(minimum_bits)
{
    for (let i = 0; i < G_ALLOWED_RSA_KEY_BITS.length; i++)
    {
        if (G_ALLOWED_RSA_KEY_BITS[i] >= minimum_bits) { return G_ALLOWED_RSA_KEY_BITS[i]; }
    }
    return 0;
}

/**
 * @brief the server rejected our identity key for being too small and told us what it wants
 *        offers to switch this device to a big enough key and reconnect. the announcement is
 *        optional on the server: without it we are simply dropped and this never runs
 *
 * @param number|string announced_minimum_bits -> the size from the notice, treated as untrusted input
 *
 * @return void
 */
function keys__handle_rsa_key_too_weak_notice(announced_minimum_bits)
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

    let target_bits = keys__pick_rsa_key_bits_for_minimum(minimum_bits);

    if (target_bits == 0)
    {
        utils__custom_alert("this server requires an RSA key of " + minimum_bits + " bits, which this client cannot create");
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
 * @brief rebuilds g_metadata_keys by sha256-hashing the predefined (or form-entered) key strings, then hands the key list to the data-processing worker
 *
 * @return void
 */
function keys__init_keys_object()
{
    g_metadata_keys.length = 0; // clear it in case client is reconnecting
    g_media_keys_from_host = null;
    g_media_server_key_layers_from_host = null;

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

/**
 * @brief diffie-hellman first step: draws a fresh secret exponent into g_dh_secret_exponent
 *        the exponent comes from the cryptographically secure RNG; 256 bits for a 2048-bit modulus, else 512
 *
 * @return bigint the public mix g^x mod p
 */
function keys__get_public_mix()
{
    // the secret exponent: 256 bits for a 2048-bit modulus, else 512. an N-bit exponent gives N/2 bits
    // of security, ample against the modulus's own strength (112 to 192 bits); bigger only slows modPow
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

    let A = lemon_crypto.modpow(g_dh_generator, g_dh_secret_exponent, g_dh_modulus);

    return A;
}

/**
 * @brief AES-CTR encrypts a byte buffer with every key layer in order
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
 * @brief the reverse of keys__encrypt_data_with_aes_keys: applies the key layers in reverse order
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
 * @brief encrypts a text with a set of keys for a channel message or a channel picture
 *
 *        the text is written into a buffer of at least 1024 bytes that is otherwise zero, so a short message
 *        and a long one look alike on the wire. then every key of the set is applied as one AES-CTR layer.
 *        every message gets its own random 16 byte starting value, the message iv. without it, two messages
 *        encrypted with the same key would use the same keystream, and anybody holding both could combine
 *        them so that the encryption cancels out; the zero padding would even expose the keystream directly.
 *        the message iv is not secret: it travels next to the ciphertext, and a receiver combines it with the
 *        key's own iv to get the starting value for that key (keys__media_xor_blocks).
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
 * @brief the reverse of keys__encrypt_with_aes_keys_and_convert_to_base64: the key layers come off in reverse order, the text is cut at the first zero byte
 *
 *        a message from before the message iv existed is a bare base64 text, not a JSON text starting with
 *        "{"; such a message was encrypted with each key's own iv as the starting value, and is decrypted
 *        that way, so stored old messages still open
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
 * @brief sha256 of a byte array, the primitive under the HKDF and HMAC below
 *        the DH metadata layer MUST match the server (mbedtls hkdf/md); js-sha256's built-in
 *        .hmac is broken in this build, so HMAC is hand-rolled on the plain hash
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
    // normalize to plain arrays: aesjs.utils.utf8.toBytes returns a Uint8Array, which has no .push,
    // and Array.concat(typedArray) appends it as ONE element instead of spreading its bytes.
    let k = Array.from(key_bytes);
    let msg = Array.from(msg_bytes);
    if (k.length > 64) { k = keys___sha256_bytes(k); }
    while (k.length < 64) { k.push(0); }
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
 * @brief derives the DH layer's AES key and HMAC key from the shared-secret decimal string
 *        salt and info MUST match _base_internal__derive_dh_keys in base.c on the server
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

// Read the type stored in a metadata key and use the encryption method that
// belongs to it. If another method is added, the server must support it too,
// and both ends must build its encryption starting value in the same way.
// Refuse an unknown type instead of silently treating its bytes as an AES key.
function keys__crypt_metadata_layer(key, message_iv, data, encrypt)
{
    switch (key.info)
    {
        case "aes-ctr":
        {
            if (key.key_bytes.length !== 32 || message_iv.length !== 16)
            {
                throw new Error("Invalid AES-256-CTR metadata key or IV");
            }
            let cipher = new aesjs.ModeOfOperation.ctr(key.key_bytes, new aesjs.Counter(Array.from(message_iv)));
            return encrypt ? cipher.encrypt(data) : cipher.decrypt(data);
        }
        default:
        {
            throw new Error("Unsupported metadata cipher: " + key.info);
        }
    }
}

/**
 * @brief wraps an outgoing message for the wire
 *        pads, encrypts with every typed metadata key under a fresh random IV, base64s, and
 *        builds the { iv, data [, tag] } envelope; on the loopback (a ui-only runtime) the text
 *        stays plain, node wraps it toward the real server
 *
 * @param string string_to_encrypt -> the message json
 *
 * @return string the envelope json, or the plaintext itself on the loopback
 */
function keys__encrypt_all_message_data_and_convert_to_base64(string_to_encrypt)
{
    // one log line saying who encrypted what: a message must be wrapped exactly once (the webview
    // leaves it plain, node wraps it), and this shows a double-wrapped or bare one
    if (typeof utils__custom_log === "function" && string_to_encrypt.indexOf("direct_chat_message") !== -1)
    {
        utils__custom_log("[crypto] direct message, ui_only=" + android_host__is_ui_only_runtime()
            + " loopback_port=" + g_loopback_port + " -> " + (android_host__is_ui_only_runtime() ? "left plain" : "wrapped"));
    }

    // loopback stays plaintext on-device; node re-encrypts toward the real server
    if (android_host__is_ui_only_runtime())
    {
        return string_to_encrypt;
    }

    let textBytes = aesjs.utils.utf8.toBytes(string_to_encrypt);
    let byteArrayToSend = new Uint8Array(Math.max(1024, textBytes.length));
    byteArrayToSend.set(textBytes);

    // fresh random IV per message (CSPRNG). the same IV seeds every metadata layer's AES-CTR counter;
    // it is not secret, so it travels as a separate "iv" field in the JSON envelope next to the ciphertext.
    let message_iv = new Uint8Array(16);
    crypto.getRandomValues(message_iv);

    let encrypted_data_in_current_iteration = byteArrayToSend;

    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        // The key made from this connection's shared secret is first in the
        // list. Encrypt with it first, then with the keys from the server settings.
        encrypted_data_in_current_iteration = keys__crypt_metadata_layer(
            g_metadata_keys[i], message_iv, encrypted_data_in_current_iteration, true);
    }

    // base64 the result after all layers. with 0 metadata keys this is just the plaintext bytes,
    // matching the server which base64-decodes and applies no cipher when keys_count is 0
    let base64EncryptedData = utils__bytesToBase64String(encrypted_data_in_current_iteration);

    let iv_base64 = utils__bytesToBase64String(message_iv);
    let envelope = { iv: iv_base64, data: base64EncryptedData };

    // encrypt-then-MAC: if a metadata key carries an HMAC key (the DH layer), authenticate iv || data
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
 * @brief the reverse of keys__encrypt_all_message_data_and_convert_to_base64
 *        verifies the HMAC tag when the DH layer is active, then peels the metadata key layers
 *
 * @param string envelope_json -> the { iv, data [, tag] } envelope from the wire
 *
 * @return string the plaintext, "" on any failure
 */
function keys__decrypt_message_metadata(envelope_json)
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
        if (g_metadata_keys[i].mac_bytes)
        {
            dh_mac_key = g_metadata_keys[i].mac_bytes;
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
        // The server encrypts with its configured keys first and this client's
        // connection key last. Undo the connection's encryption first, then
        // use the configured keys from last to first. Keep that order so this
        // also works with methods where changing the key order changes the result.
        for (let i = g_metadata_keys.length - 1; i >= 0; i--)
        {
            if (g_metadata_keys[i].mac_bytes)
            {
                current = keys__crypt_metadata_layer(g_metadata_keys[i], message_iv, current, false);
            }
        }
        for (let i = g_metadata_keys.length - 1; i >= 0; i--)
        {
            if (!g_metadata_keys[i].mac_bytes)
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
 * @brief arms (re-arms) the keys-wait timer for the current channel
 *        no-op when there is nothing to wait for: no maintainer announced, or the local user IS
 *        the maintainer; node owns this decision, the webview must not vote
 *
 * @return void
 */
function keys__arm_maintainer_keys_wait_timer()
{
    // node owns every reactive protocol decision, this one included. the webview voting
    // too churned the maintainer in a loop and invalidated the keys it had just applied
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
 * @brief stops the pending keys-wait timer, if any, and clears maintainer_keys_wait_timer
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
 * @brief the maintainer stayed silent for the whole timeout: sends the reset vote and re-arms
 *        every condition is re-checked, because a channel switch, arriving keys or a new
 *        maintainer may have happened meanwhile
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

    // payload-free: the client just asks for its channel's maintainer to be reset,
    // the server does all vote bookkeeping (one vote per client, dies on maintainer change)
    let message_object = {
        message: {
            type: "reset_channel_maintainer"
        }
    };

    connection__send_message_object(message_object);

    // keep voting while still keyless: the server counts at most one vote per client,
    // so repeats are harmless and cover a lost request on the way
    maintainer_keys_wait_timer = setTimeout(keys__maintainer_keys_wait_timer_fired, MAINTAINER_KEYS_WAIT_TIMEOUT_MS);
}

/**
 * @brief asks the data-processing worker to create a fresh channel key set and send it to every member, encrypted for each one's public key
 *        reactive protocol runs in ONE runtime, node: the webview reacting too sent a second key
 *        set on the same connection and the server kicked it
 *
 * @return void
 */
function keys__create_and_send_new_channel_keys()
{
    // reactive protocol runs in ONE runtime: node. the webview reacting too sent a
    // second key set on the same connection and the server kicked it
    if (android_host__is_ui_only_runtime())
    {
        return;
    }

    g_data_processing_worker.postMessage({
        type: "mainthread__create_websocket_channel_keys_message",
        clients: g_client_list,
        local_client_id: g_local_client_id,
        current_channel_id: g_current_channel_id
    });

    // rsa stays in the data-processing worker (the private key lives only there), so each public key
    // is posted to it and the encrypted keys come back for the main thread to route
}

// ================================================================================================
// the encryption of audio and video frames on the way between this client and the server
// ================================================================================================
//
// why this exists: a voice frame or a video packet is encrypted with the channel keys, which only the
// members of the channel have, so the server cannot listen in. two things are still open on the way to
// the server. first, a few bytes next to the channel key encryption, for example who sent the frame, can
// be read by anybody on the network path when the connection has no TLS. second, nothing proves that a
// frame really comes from the client it claims to come from: somebody on the path could change a frame or
// insert one. so every frame is encrypted a second time, with the two keys this client and the server
// derived from the Diffie-Hellman shared secret at login (the encryption key and the checksum key, the
// same ones that encrypt the JSON messages), and an authentication code is appended: a checksum that only
// somebody with the checksum key can compute, HMAC-SHA256. the server checks the code, removes this
// client's encryption, and encrypts the frame again for every receiver with the receiver's keys. the
// server side is media_auth.c, which also explains why every frame needs a counter.
//
// a frame on the wire:           [1 byte version 2][8 bytes frame counter][encrypted part][8 bytes authentication code]
// the encrypted part, decrypted: [4 bytes sender id, least significant byte first][8 bytes sender counter][the inner ciphertext]
// the inner ciphertext is the frame content under the channel keys (or a music bot's keys) and then under
// the server keys. the counter block (the 16 byte starting value of AES-CTR) of every inner layer is
// [4 bytes sender id][8 bytes sender counter][4 bytes zero], combined by XOR with the key's iv when the key
// has one. the counter block of the second encryption is [8 bytes frame counter][1 byte direction: 0 to
// the server, 1 from the server][1 byte stream: 0 audio, 1 video][6 bytes zero].
var MEDIA_FRAME_VERSION = 2;
var MEDIA_FRAME_HEADER = 9;
var MEDIA_BODY_HEADER = 12;
var MEDIA_FRAME_CODE = 8;
var MEDIA_FRAME_OVERHEAD = MEDIA_FRAME_HEADER + MEDIA_BODY_HEADER + MEDIA_FRAME_CODE;
var MEDIA_STREAM_AUDIO = 0;
var MEDIA_STREAM_VIDEO = 1;
var MEDIA_DIRECTION_TO_SERVER = 0;
var MEDIA_DIRECTION_FROM_SERVER = 1;

// on android the node runtime hands the session and configured server keys to the webview over loopback
var g_media_keys_from_host = null;
var g_media_server_key_layers_from_host = null;

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
 * @brief keeps the complete media key set the host runtime handed over (android: node to webview)
 *
 * @param string encryption_key_hex -> the 32 byte encryption key as hex
 * @param string checksum_key_hex -> the 32 byte checksum key as hex
 * @param array server_key_hexes -> the ordered 32 byte server keys as hex
 *
 * @return void
 */
function keys__set_media_keys_from_host(encryption_key_hex, checksum_key_hex, server_key_hexes = [])
{
    let key_pattern = /^[0-9a-f]{64}$/i;
    if (typeof encryption_key_hex !== "string" || typeof checksum_key_hex !== "string"
        || !key_pattern.test(encryption_key_hex) || !key_pattern.test(checksum_key_hex) || !Array.isArray(server_key_hexes))
    {
        return;
    }
    let server_layers = [];
    for (let i = 0; i < server_key_hexes.length; i++)
    {
        if (typeof server_key_hexes[i] !== "string" || !key_pattern.test(server_key_hexes[i]))
        {
            return;
        }
        server_layers.push(
        {
            key_bytes: keys__hex_to_bytes(server_key_hexes[i]),
            iv_bytes: null
        });
    }
    g_media_keys_from_host =
    {
        key_bytes: keys__hex_to_bytes(encryption_key_hex),
        mac_bytes: keys__hex_to_bytes(checksum_key_hex)
    };
    g_media_server_key_layers_from_host = server_layers;
}

/**
 * @brief the two keys of this client for the media frames
 *
 *        the Diffie-Hellman entry of g_metadata_keys is the one that carries a checksum key (mac_bytes);
 *        on android the webview has none and uses the keys the node runtime handed over
 *
 * @return object { key_bytes, mac_bytes } or null before the key exchange
 */
function keys__media_session_layer()
{
    for (let i = 0; i < g_metadata_keys.length; i++)
    {
        if (g_metadata_keys[i].mac_bytes)
        {
            return { key_bytes: g_metadata_keys[i].key_bytes, mac_bytes: g_metadata_keys[i].mac_bytes };
        }
    }
    return g_media_keys_from_host;
}

/**
 * @brief the server keys as layers for the inner encryption of media frames: every entry of g_metadata_keys without a checksum key
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
        if (!g_metadata_keys[i].mac_bytes)
        {
            layers.push({ key_bytes: g_metadata_keys[i].key_bytes, iv_bytes: null });
        }
    }
    return layers;
}

/**
 * @brief a fresh frame counter: 8 random bytes, so the counters of this session differ from those of any earlier session
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
 * @brief adds one to a frame counter, in place; the last byte is the least significant one
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
 * @brief combines two 16 byte blocks with XOR; a missing block counts as zero
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
 * @brief the counter block of an inner layer: the sender id, the sender counter, and the key's own iv when it has one
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
 * @brief the counter block of the second encryption, the one between this client and the server
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
 * @brief one AES-CTR pass over bytes; the same call encrypts and decrypts
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
 * @brief applies the inner layers (channel keys, a music bot's keys, or the server keys) to the content of a frame
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
        current = keys__aes_ctr(layers[i].key_bytes, keys__media_inner_block(sender_id, counter_bytes, layers[i].iv_bytes), current);
    }
    return current;
}

/**
 * @brief puts the sender id and the sender counter in front of the inner ciphertext: that is the part the second encryption covers
 *
 * @return Uint8Array [4 bytes sender id][8 bytes counter][inner]
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
 * @brief the second encryption of a frame this client sends: the body is encrypted with the encryption key and the authentication code is appended
 *
 * @param object session_layer -> { key_bytes, mac_bytes } from keys__media_session_layer
 * @param number stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param Uint8Array counter_bytes -> the frame counter, 8 bytes, increased before every frame
 * @param Uint8Array body -> from keys__media_build_body
 *
 * @return Uint8Array the frame for the wire, or null without keys
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
    frame.set(keys__aes_ctr(session_layer.key_bytes, keys__media_session_block(counter_bytes, MEDIA_DIRECTION_TO_SERVER, stream), body), MEDIA_FRAME_HEADER);
    let code = keys___hmac_sha256(session_layer.mac_bytes, frame.subarray(0, MEDIA_FRAME_HEADER + body.length));
    frame.set(code.slice(0, MEDIA_FRAME_CODE), MEDIA_FRAME_HEADER + body.length);
    return frame;
}

/**
 * @brief a frame that came from the server: the authentication code is checked and the second encryption removed
 *
 *        the code is compared byte by byte without stopping early, so the time the comparison takes tells
 *        nothing about where the first difference is
 *
 * @param object session_layer -> { key_bytes, mac_bytes } from keys__media_session_layer
 * @param number stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param Uint8Array frame -> the frame from the wire
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
    let expected = keys___hmac_sha256(session_layer.mac_bytes, frame.subarray(0, code_offset));
    let difference = 0;
    for (let i = 0; i < MEDIA_FRAME_CODE; i++)
    {
        difference |= expected[i] ^ frame[code_offset + i];
    }
    if (difference !== 0)
    {
        return null;
    }
    let body = keys__aes_ctr(session_layer.key_bytes, keys__media_session_block(frame.subarray(1, MEDIA_FRAME_HEADER), MEDIA_DIRECTION_FROM_SERVER, stream), frame.subarray(MEDIA_FRAME_HEADER, code_offset));
    return {
        sender_id: (body[0] | (body[1] << 8) | (body[2] << 16) | (body[3] << 24)) >>> 0,
        counter_bytes: body.slice(4, MEDIA_BODY_HEADER),
        inner: body.subarray(MEDIA_BODY_HEADER)
    };
}
