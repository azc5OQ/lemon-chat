/**
 * @brief decodes UTF8 while preserving a passphrase's leading BOM
 *
 * @param Uint8Array|ArrayBuffer bytes -> file contents, at most 65536 bytes
 *
 * @return string decoded text. invalid UTF8 throws
 */
function identity_file__decode(bytes)
{
    const input = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (input.byteLength > 65536)
    {
        throw new Error("Choose an identity file no larger than 64 KB.");
    }
    try
    {
        return new TextDecoder("utf-8", {fatal: true, ignoreBOM: true}).decode(input);
    }
    catch (error)
    {
        throw new Error("This file contains invalid UTF-8 text. The identity was not changed.");
    }
}

/**
 * @brief validates the identity format, key size and passphrase
 *
 * @param object record -> parsed identity file
 *
 * @return object the passphrase, RSA size and legacy flag
 */
function identity_file__validate(record)
{
    if (record === null || typeof record !== "object" || Array.isArray(record) || record.format !== "lemon-chat-identity")
    {
        throw new Error("This file is not a Lemon Chat identity.");
    }
    if (record.version !== 1)
    {
        throw new Error("This identity file version is not supported by this client.");
    }
    if (!Number.isInteger(record.rsa_key_bits) || !G_ALLOWED_RSA_KEY_BITS.includes(record.rsa_key_bits))
    {
        throw new Error("This identity file uses an unsupported RSA key size.");
    }
    if (typeof record.passphrase !== "string" || record.passphrase.length < 199)
    {
        throw new Error("This identity file does not contain a complete passphrase.");
    }
    return {passphrase: record.passphrase, rsa_key_bits: record.rsa_key_bits, legacy: false};
}

/**
 * @brief parses an identity file without trimming the passphrase
 *
 * @param string content -> file text, at most 65536 UTF8 bytes
 * @param boolean explicit_legacy -> true to treat all characters as a raw passphrase
 *
 * @return object passphrase, RSA size and legacy format flag
 */
function identity_file__parse(content, explicit_legacy = false)
{
    if (typeof content !== "string" || new TextEncoder().encode(content).length > 65536)
    {
        throw new Error("Choose an identity file no larger than 64 KB.");
    }
    const formatted = content.replace(/^\uFEFF/, "").trim();
    const looks_like_json = /^[{[\"]/.test(formatted);
    if (explicit_legacy || (!looks_like_json && !content.startsWith("\uFEFF")))
    {
        if (content.length < 199)
        {
            throw new Error("This identity file does not contain a complete passphrase.");
        }
        return {passphrase: content, rsa_key_bits: null, legacy: true};
    }
    let record;
    try
    {
        record = JSON.parse(formatted);
    }
    catch (error)
    {
        throw new Error("This is not a valid identity file. For an older raw passphrase beginning with a BOM or JSON punctuation, use Paste a passphrase instead; preserve every character.");
    }
    return identity_file__validate(record);
}

/**
 * @brief exports the passphrase with the RSA size needed to rebuild its identity
 *
 * @param string passphrase -> identity seed
 * @param number rsa_key_bits -> original key generation size
 *
 * @return string validated identity JSON
 */
function identity_file__serialize(passphrase, rsa_key_bits)
{
    const record =
    {
        format: "lemon-chat-identity",
        version: 1,
        rsa_key_bits: rsa_key_bits,
        passphrase: passphrase
    };
    identity_file__validate(record);
    const content = JSON.stringify(record, null, 2) + "\n";
    if (new TextEncoder().encode(content).length > 65536)
    {
        throw new Error("This passphrase is too large for an identity file. Keep a copy of the passphrase and its key size instead.");
    }
    return content;
}

/**
 * @brief finds the supported generation size from a public modulus
 *
 * @param string public_key -> encoded public key
 *
 * @return number supported RSA size, or zero when it cannot be determined
 */
function identity_file__generation_size(public_key)
{
    try
    {
        const modulus_bits = lemon_crypto.publicKeyFromString(public_key).n.toString(2).length;
        for (const bits of G_ALLOWED_RSA_KEY_BITS)
        {
            if (modulus_bits === bits || modulus_bits === bits - 1)
            {
                return bits;
            }
        }
    }
    catch (error)
    {
        return 0;
    }
    return 0;
}

/**
 * @brief restores a saved identity or requests recovery when its size is unknown
 *
 * @param string|null stored_record -> saved JSON identity
 * @param string|null legacy_seed -> older passphrase without a saved key size
 *
 * @return object ready, recovery or missing state
 */
function identity_file__read_saved(stored_record, legacy_seed)
{
    if (stored_record != null)
    {
        try
        {
            const record = JSON.parse(stored_record);
            const validated = identity_file__validate(
            {
                format: "lemon-chat-identity",
                version: 1,
                passphrase: record.passphrase,
                rsa_key_bits: record.rsa_key_bits
            });
            return {kind: "ready", passphrase: validated.passphrase, rsa_key_bits: validated.rsa_key_bits};
        }
        catch (error)
        {
            return {kind: "recovery", passphrase: null,
                reason: "The saved identity could not be read. It has been kept unchanged. Import your identity backup to continue."};
        }
    }
    if (legacy_seed != null)
    {
        return {
            kind: "recovery",
            passphrase: typeof legacy_seed === "string" && legacy_seed.length >= 199 ? legacy_seed : null,
            reason: typeof legacy_seed === "string" && legacy_seed.length >= 199 ? "This older saved identity has no key size. Select its original size below, then import it. A different size creates a different identity. Your saved passphrase has been kept unchanged." : "The saved passphrase is incomplete. It has been kept unchanged. Import your identity backup to continue."
        };
    }
    return {kind: "missing", passphrase: null};
}
