// The browser, Android node host and native server share these cipher implementations.
var connection_crypto_wasm = null;
var CONNECTION_CIPHERS = ["aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"];
var CONNECTION_CIPHER_IDS = [1, 3, 4, 5, 6, 7];

/** @brief Loads the local embedded module; it has no imports or external requests. */
function connection_crypto__module()
{
    if (connection_crypto_wasm == null)
    {
        let encoded = '@@WASM:wasm/connection_crypto.wasm@@';
        let binary = atob(encoded);
        let bytes = Uint8Array.from(binary, function(character)
        {
            return character.charCodeAt(0);
        });
        connection_crypto_wasm = new WebAssembly.Instance(new WebAssembly.Module(bytes), {}).exports;
    }
    return connection_crypto_wasm;
}

/** @brief Encrypts/decrypts a typed 256-bit layer, clearing the temporary WASM memory. */
function connection_crypto__crypt(type, key, iv, data)
{
    let index = CONNECTION_CIPHERS.indexOf(type);
    if (index < 0 || key.length !== 32 || iv.length !== 16)
    {
        throw new Error("Unsupported connection cipher or invalid key/IV");
    }
    let module = connection_crypto__module();
    let base = module.__heap_base.value;
    let required = base + 64 + data.length;
    if (required > module.memory.buffer.byteLength)
    {
        module.memory.grow(Math.ceil((required - module.memory.buffer.byteLength) / 65536));
    }
    let memory = new Uint8Array(module.memory.buffer);
    memory.set(key, base);
    memory.set(iv, base + 32);
    memory.set(data, base + 64);
    try
    {
        if (module.ciphers__crypt(CONNECTION_CIPHER_IDS[index], base, base + 32, base + 64, data.length) !== 1)
        {
            throw new Error("Connection cipher failed");
        }
        return memory.slice(base + 64, required);
    }
    finally
    {
        memory.fill(0, base, required);
    }
}

/** @brief Normalizes legacy password strings and typed password objects. */
function connection_crypto__password_entry(value)
{
    let entry = typeof value === "string" ? { type: "aes256", password: value } : value;
    if (entry == null || CONNECTION_CIPHERS.indexOf(entry.type) < 0 || typeof entry.password !== "string")
    {
        throw new Error("Invalid connection password or cipher type");
    }
    return { type: entry.type, password: entry.password };
}

/** @brief Matches the C length-prefixed hash-chain for handshake fields. */
function connection_crypto__mix(state, value)
{
    let bytes = typeof value === "string" ? aesjs.utils.utf8.toBytes(value) : value;
    let length = bytes.length;
    return _sha256.array(Array.from(state).concat([(length >>> 24) & 255, (length >>> 16) & 255,
        (length >>> 8) & 255, length & 255], Array.from(bytes)));
}

/** @brief Derives a purpose-specific 256-bit key with the agreed transcript as HKDF salt. */
function connection_crypto__derive(secret, transcript, purpose)
{
    return keys___hkdf_sha256(secret, transcript, aesjs.utils.utf8.toBytes(purpose), 32);
}

/** @brief Validates a negotiated policy before any additional expensive DH work. */
function connection_crypto__validate_policy(policy)
{
    if (policy == null || policy.version !== 2 || !/^[0-9a-f]{32}$/.test(policy.nonce)
        || !Number.isInteger(policy.dh_exchange_count) || policy.dh_exchange_count < 1 || policy.dh_exchange_count > 4
        || !Number.isInteger(policy.session_encryption_layer_count) || policy.session_encryption_layer_count < 1 || policy.session_encryption_layer_count > 8
        || CONNECTION_CIPHERS.indexOf(policy.session_cipher) < 0 || typeof policy.is_random_cipher_layer_enabled !== "boolean")
    {
        throw new Error("Unsupported or excessive server encryption policy");
    }
}

var g_crypto_handshake = null;

/** @brief Starts one bounded negotiation; each exchange keeps its own fresh exponent. */
function connection_crypto__begin(public_key, mix)
{
    g_crypto_handshake = { public_key: public_key, mixes: [mix], exponents: [g_dh_secret_exponent], policy: null };
}

function connection_crypto__accept_policy(policy)
{
    connection_crypto__validate_policy(policy);
    let state = g_crypto_handshake;
    if (state == null || state.policy != null)
    {
        throw new Error("Unexpected encryption policy");
    }
    state.policy = policy;
    while (state.mixes.length < policy.dh_exchange_count)
    {
        state.mixes.push(keys__get_public_mix().toString());
        state.exponents.push(g_dh_secret_exponent);
    }
    return { message:
    {
        type: "public_key_info", value: state.public_key, verification_string: VERIFICATION_MESSAGE,
        dh_public_mix: state.mixes[0], dh_public_mixes: state.mixes,
        crypto_version: 2, crypto_nonce: policy.nonce
    } };
}

function connection_crypto__hex(value, length)
{
    if (typeof value !== "string" || value.length !== length * 2 || !/^[0-9a-f]+$/.test(value))
    {
        throw new Error("Invalid handshake proof");
    }
    return keys__hex_to_bytes(value);
}

function connection_crypto__equal(left, right)
{
    if (left.length !== right.length)
    {
        return false;
    }
    let difference = 0;
    for (let index = 0; index < left.length; index++)
    {
        difference |= left[index] ^ right[index];
    }
    return difference === 0;
}

/** @brief Verifies the negotiated transcript and optional hidden-cipher probe before installing keys. */
function connection_crypto__finish(message)
{
    let state = g_crypto_handshake;
    if (state == null)
    {
        throw new Error("Unexpected encryption challenge");
    }
    let secret = new Uint8Array(32);
    try
    {
        if (state.policy == null)
        {
            if (message.crypto != null)
            {
                throw new Error("Missing encryption policy");
            }
            let mix = connection_crypto__public_mix(message.dh_public_mix);
            let shared = lemon_crypto.modpow(mix, state.exponents[0], g_dh_modulus).toString();
            let keys = keys__dh_derive_keys(shared);
            return [{ info: "aes-ctr", key_bytes: keys.enc_key, mac_bytes: keys.mac_key }];
        }
        let policy = state.policy;
        let reply = message.crypto;
        if (reply == null || reply.version !== 2 || !Array.isArray(reply.dh_public_mixes)
            || reply.dh_public_mixes.length !== policy.dh_exchange_count || reply.dh_public_mixes[0] !== message.dh_public_mix)
        {
            throw new Error("Incomplete encryption negotiation");
        }
        let transcript = connection_crypto__mix(new Uint8Array(32), "lemonchat-v2|" + policy.nonce + "|"
            + policy.dh_exchange_count + "|" + policy.session_encryption_layer_count + "|" + policy.session_cipher
            + "|" + (policy.is_random_cipher_layer_enabled ? 1 : 0));
        transcript = connection_crypto__mix(transcript, state.public_key);
        for (let index = 0; index < policy.dh_exchange_count; index++)
        {
            let mix = connection_crypto__public_mix(reply.dh_public_mixes[index]);
            let shared = lemon_crypto.modpow(mix, state.exponents[index], g_dh_modulus).toString();
            let next = connection_crypto__mix(secret, shared);
            secret.fill(0);
            secret = new Uint8Array(next);
            transcript = connection_crypto__mix(transcript, state.mixes[index]);
            transcript = connection_crypto__mix(transcript, reply.dh_public_mixes[index]);
        }
        let random_type = null;
        if (policy.is_random_cipher_layer_enabled)
        {
            let probe = reply.random_probe;
            if (probe == null)
            {
                throw new Error("Missing random cipher probe");
            }
            let iv = connection_crypto__hex(probe.iv, 16);
            let data = connection_crypto__hex(probe.data, 32);
            let tag = connection_crypto__hex(probe.tag, 32);
            let check_key = connection_crypto__derive(secret, transcript, "lemonchat/v2/random-cipher/probe-check");
            let matches = 0;
            for (let type of CONNECTION_CIPHERS)
            {
                let key = connection_crypto__derive(secret, transcript, "lemonchat/v2/random-cipher/probe/" + type);
                let plaintext = connection_crypto__crypt(type, key, iv, data);
                let expected = keys___hmac_sha256(check_key, Array.from(plaintext).concat(transcript));
                if (connection_crypto__equal(tag, expected))
                {
                    random_type = type;
                    matches++;
                }
                key.fill(0);
                plaintext.fill(0);
            }
            check_key.fill(0);
            if (matches !== 1)
            {
                throw new Error("Random cipher proof failed");
            }
        }
        else if (reply.random_probe != null)
        {
            throw new Error("Unexpected random cipher layer");
        }
        let receive_mac = connection_crypto__derive(secret, transcript, "lemonchat/v2/server-to-client/mac");
        let expected = keys___hmac_sha256(receive_mac, transcript);
        if (!connection_crypto__equal(expected, connection_crypto__hex(reply.finished, 32)))
        {
            receive_mac.fill(0);
            throw new Error("Server encryption proof failed");
        }
        let layers = [];
        let count = policy.session_encryption_layer_count + (random_type == null ? 0 : 1);
        for (let index = 0; index < count; index++)
        {
            let type = index < policy.session_encryption_layer_count ? policy.session_cipher : random_type;
            layers.push(
            {
                info: type, key_type: type, session_layer: true,
                key_bytes: connection_crypto__derive(secret, transcript, "lemonchat/v2/client-to-server/layer/" + index + "/" + type),
                receive_key_bytes: connection_crypto__derive(secret, transcript, "lemonchat/v2/server-to-client/layer/" + index + "/" + type)
            });
        }
        layers[0].mac_bytes = connection_crypto__derive(secret, transcript, "lemonchat/v2/client-to-server/mac");
        layers[0].receive_mac_bytes = receive_mac;
        return layers;
    }
    finally
    {
        secret.fill(0);
        state.exponents.fill(null);
        g_dh_secret_exponent = null;
        g_crypto_handshake = null;
    }
}

function connection_crypto__public_mix(value)
{
    if (typeof value !== "string" || value.length > 4096 || !/^[0-9]+$/.test(value))
    {
        throw new Error("Invalid DH public mix");
    }
    let mix = BigInt(value);
    if (mix <= 1n || mix >= g_dh_modulus - 1n)
    {
        throw new Error("Degenerate DH public mix");
    }
    return mix;
}

/** @brief Drops private handshake references when a socket closes before negotiation finishes. */
function connection_crypto__clear_handshake()
{
    if (g_crypto_handshake != null)
    {
        g_crypto_handshake.exponents.fill(null);
        g_crypto_handshake = null;
    }
    g_dh_secret_exponent = null;
}
