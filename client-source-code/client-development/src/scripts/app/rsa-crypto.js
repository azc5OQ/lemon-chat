// rsa-crypto.js is embedded in template.html along with the other client files, and in the node bundle
// it is the client's rsa layer (lemon_crypto): keypair from a passphrase, encrypt, decrypt and the pkcs#1
// challenge, with the modular exponentiation in rsa_keygen.wasm and everything else native BigInt
// every wire format is byte-identical to the old cryptico stack, so identities and messages stay compatible
// identity-crypto.js, worker-data-processing.js and chat-files.js call it

var rsa_keygen_wasm_exports = null;

/**
 * @brief the rsa keygen wasm, compiled on first use
 *        the module imports nothing and is ~23 KB. synchronous compilation is fine because every
 *        rsa caller lives in the data-processing worker; the main thread only ever uses the
 *        diffie-hellman helper, which needs no wasm
 *
 * @return object the wasm exports
 */
function rsa_crypto__get_rsa_keygen_wasm()
{
    if (rsa_keygen_wasm_exports == null)
    {
        var rsa_keygen_webassembly_base64 = '@@WASM:wasm/rsa_keygen.wasm@@';
        var binary_string = atob(rsa_keygen_webassembly_base64);
        var module_bytes = new Uint8Array(binary_string.length);

        for (var i = 0; i < binary_string.length; i++)
        {
            module_bytes[i] = binary_string.charCodeAt(i);
        }

        var wasm_module = new WebAssembly.Module(module_bytes.buffer);
        rsa_keygen_wasm_exports = new WebAssembly.Instance(wasm_module, {}).exports;
    }
    return rsa_keygen_wasm_exports;
}

var lemon_crypto = (function ()
{
    var my = {};

    var base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    var hex_chars = "0123456789abcdef";

    // marks a signed payload inside the envelope. lemon-chat never signs, but the
    // separator is still split on so a signed message from any other cryptico client
    // yields the same plaintext it always did
    var signature_separator = "::52cee64bb3a38f6403386519a39ac91c::";

    // conversions, ported unchanged from cryptico: the envelope is built out of these, so a single
    // differing character would break older clients

    /**
     * @brief binary string (one character per byte) -> base64
     *
     * @param string t -> the binary string
     *
     * @return string the base64 text
     */
    my.b256to64 = function (t)
    {
        var a, c, n;
        var r = "", s = 0;
        var tl = t.length;

        for (n = 0; n < tl; n++)
        {
            c = t.charCodeAt(n);
            if (s == 0)
            {
                r += base64_chars.charAt((c >> 2) & 63);
                a = (c & 3) << 4;
            }
            else if (s == 1)
            {
                r += base64_chars.charAt((a | (c >> 4) & 15));
                a = (c & 15) << 2;
            }
            else if (s == 2)
            {
                r += base64_chars.charAt(a | ((c >> 6) & 3));
                r += base64_chars.charAt(c & 63);
            }
            s += 1;
            if (s == 3) { s = 0; }
        }

        if (s > 0)
        {
            r += base64_chars.charAt(a);
            r += "=";
        }
        if (s == 1)
        {
            r += "=";
        }
        return r;
    };

    /**
     * @brief base64 -> binary string (one character per byte)
     *
     * @param string t -> the base64 text
     *
     * @return string the binary string
     */
    my.b64to256 = function (t)
    {
        var c, n;
        var r = "", s = 0, a = 0;
        var tl = t.length;

        for (n = 0; n < tl; n++)
        {
            c = base64_chars.indexOf(t.charAt(n));
            if (c >= 0)
            {
                if (s) { r += String.fromCharCode(a | (c >> (6 - s)) & 255); }
                s = (s + 2) & 7;
                a = (c << s) & 255;
            }
        }
        return r;
    };

    /**
     * @brief hex -> base64
     *
     * @param string h -> the hex text
     *
     * @return string the base64 text
     */
    my.b16to64 = function (h)
    {
        var i, c;
        var ret = "";

        if (h.length % 2 == 1) { h = "0" + h; }

        for (i = 0; i + 3 <= h.length; i += 3)
        {
            c = parseInt(h.substring(i, i + 3), 16);
            ret += base64_chars.charAt(c >> 6) + base64_chars.charAt(c & 63);
        }
        if (i + 1 == h.length)
        {
            c = parseInt(h.substring(i, i + 1), 16);
            ret += base64_chars.charAt(c << 2);
        }
        else if (i + 2 == h.length)
        {
            c = parseInt(h.substring(i, i + 2), 16);
            ret += base64_chars.charAt(c >> 2) + base64_chars.charAt((c & 3) << 4);
        }
        while ((ret.length & 3) > 0) { ret += "="; }
        return ret;
    };

    /**
     * @brief base64 -> hex
     *
     * @param string s -> the base64 text
     *
     * @return string the hex text
     */
    my.b64to16 = function (s)
    {
        var ret = "";
        var i, v;
        var k = 0;
        var slop = 0;

        for (i = 0; i < s.length; ++i)
        {
            if (s.charAt(i) == "=") { break; }
            v = base64_chars.indexOf(s.charAt(i));
            if (v < 0) { continue; }

            if (k == 0)
            {
                ret += hex_chars.charAt(v >> 2);
                slop = v & 3;
                k = 1;
            }
            else if (k == 1)
            {
                ret += hex_chars.charAt((slop << 2) | (v >> 4));
                slop = v & 0xf;
                k = 2;
            }
            else if (k == 2)
            {
                ret += hex_chars.charAt(slop);
                ret += hex_chars.charAt(v >> 2);
                slop = v & 3;
                k = 3;
            }
            else
            {
                ret += hex_chars.charAt((slop << 2) | (v >> 4));
                ret += hex_chars.charAt(v & 0xf);
                k = 0;
            }
        }
        if (k == 1) { ret += hex_chars.charAt(slop << 2); }
        return ret;
    };

    /**
     * @brief a string's character codes as a byte array
     *
     * @param string string -> the text
     *
     * @return array the codes
     */
    my.string2bytes = function (string)
    {
        var bytes = new Array();
        for (var i = 0; i < string.length; i++)
        {
            bytes.push(string.charCodeAt(i));
        }
        return bytes;
    };

    /**
     * @brief a byte array as a string, one character per byte
     *
     * @param array bytes -> the bytes
     *
     * @return string the text
     */
    my.bytes2string = function (bytes)
    {
        var string = "";
        for (var i = 0; i < bytes.length; i++)
        {
            string += String.fromCharCode(bytes[i]);
        }
        return string;
    };

    /**
     * @brief zero padding to a whole number of aes blocks
     *        not pkcs#7: this is the padding cryptico shipped, and the envelope format is fixed by old clients
     *
     * @param array bytes -> the plaintext bytes
     *
     * @return array a padded copy
     */
    my.pad16 = function (bytes)
    {
        var new_bytes = bytes.slice(0);
        var padding = (16 - (bytes.length % 16)) % 16;

        for (var i = 0; i < padding; i++) { new_bytes.push(0); }
        return new_bytes;
    };

    /**
     * @brief the inverse of pad16: strips the trailing zero bytes
     *
     * @param array bytes -> the padded bytes
     *
     * @return array the bytes without the padding
     */
    my.depad = function (bytes)
    {
        var end = bytes.length;
        while (end > 0 && bytes[end - 1] == 0) { end--; }
        return bytes.slice(0, end);
    };

    // random material from getRandomValues, the csprng the rest of the client uses (the old stack
    // drew it from an arcfour pool seeded with Math.random)

    /**
     * @brief random bytes from the cryptographically secure RNG
     *
     * @param number count -> how many
     *
     * @return array the bytes
     */
    function random_bytes(count)
    {
        var buffer = new Uint8Array(count);
        var bytes = new Array(count);

        crypto.getRandomValues(buffer);
        for (var i = 0; i < count; i++) { bytes[i] = buffer[i]; }
        return bytes;
    }

    /**
     * @brief a fresh random 32-byte aes key
     *
     * @return array the key bytes
     */
    my.generateAESKey = function () { return random_bytes(32); };
    /**
     * @brief a fresh random 16-byte aes iv
     *
     * @return array the iv bytes
     */
    my.blockIV = function () { return random_bytes(16); };

    // -----------------------------------------------------------------------
    // big number helpers
    // -----------------------------------------------------------------------

    /**
     * @brief a BigInt from a hex string; throws when the text is not hex
     *
     * @param string hex -> the hex text
     *
     * @return bigint the value
     */
    function bigint_from_hex(hex)
    {
        if (typeof hex !== "string" || hex.length == 0 || /[^0-9a-fA-F]/.test(hex))
        {
            throw new Error("not a hex string");
        }
        return BigInt("0x" + hex);
    }

    /**
     * @brief minimal big-endian bytes of a positive value, the shape jsbn's toByteArray produced once its leading zero byte is skipped
     *
     * @param bigint value -> the value
     *
     * @return array the bytes
     */
    function bigint_to_bytes(value)
    {
        var hex = value.toString(16);
        var bytes = [];

        if (hex.length % 2 == 1) { hex = "0" + hex; }
        for (var i = 0; i < hex.length; i += 2)
        {
            bytes.push(parseInt(hex.substring(i, i + 2), 16));
        }
        return bytes;
    }

    /**
     * @brief how many bits a value needs
     *
     * @param bigint value -> the value
     *
     * @return number the bit length, 0 for anything not positive
     */
    function bit_length(value)
    {
        if (value <= 0n) { return 0; }
        return value.toString(2).length;
    }

    /**
     * @brief base^exponent mod modulus on the wasm, in hex; the modulus must be odd, which every rsa n, p and q is
     *
     * @param string base_hex -> the base
     * @param string exponent_hex -> the exponent
     * @param string modulus_hex -> the modulus
     *
     * @return string the result in hex
     */
    function wasm_modpow_hex(base_hex, exponent_hex, modulus_hex)
    {
        var wasm = rsa_crypto__get_rsa_keygen_wasm();
        var memory_bytes = new Uint8Array(wasm.memory.buffer);
        var operands = [base_hex, exponent_hex, modulus_hex];
        var i, j;

        for (i = 0; i < 3; i++)
        {
            var input_ptr = wasm.rsa_keygen__get_modpow_buffer(i);

            if (operands[i].length > 2080) { throw new Error("modpow operand too large"); }
            for (j = 0; j < operands[i].length; j++)
            {
                memory_bytes[input_ptr + j] = operands[i].charCodeAt(j);
            }
            memory_bytes[input_ptr + operands[i].length] = 0;
        }

        if (wasm.rsa_keygen__modpow() != 1) { throw new Error("rsa_keygen__modpow rejected input"); }

        memory_bytes = new Uint8Array(wasm.memory.buffer);
        var result_ptr = wasm.rsa_keygen__get_modpow_result();
        var hex = "";
        for (j = result_ptr; memory_bytes[j] != 0; j++)
        {
            hex += String.fromCharCode(memory_bytes[j]);
        }
        return hex;
    }

    /**
     * @brief base^exponent mod modulus on the wasm, for BigInts
     *
     * @param bigint base -> the base
     * @param bigint exponent -> the exponent
     * @param bigint modulus -> the modulus, odd
     *
     * @return bigint the result
     */
    function modpow_wasm(base, exponent, modulus)
    {
        return bigint_from_hex(wasm_modpow_hex(base.toString(16), exponent.toString(16), modulus.toString(16)));
    }

    /**
     * @brief base^exponent mod modulus by square and multiply over native BigInt
     *        the diffie-hellman step runs on the main thread, which never instantiates the wasm
     *        module, so it uses this instead
     *
     * @param bigint base -> the base
     * @param bigint exponent -> the exponent
     * @param bigint modulus -> the modulus
     *
     * @return bigint the result
     */
    my.modpow = function (base, exponent, modulus)
    {
        var result = 1n;

        base = base % modulus;
        while (exponent > 0n)
        {
            if ((exponent & 1n) == 1n) { result = (result * base) % modulus; }
            base = (base * base) % modulus;
            exponent >>= 1n;
        }
        return result;
    };

    // -----------------------------------------------------------------------
    // pkcs#1 v1.5, ported from rsa.js including its utf-8 expansion of the payload
    // -----------------------------------------------------------------------

    /**
     * @brief pkcs#1 type 2 padding of a text into an n-byte block: utf8 encoding, random non-zero padding, the 0x02 marker
     *        throws when the text does not fit
     *
     * @param string s -> the text
     * @param number n -> the block length in bytes
     *
     * @return bigint the padded block as a number
     */
    function pkcs1_pad(s, n)
    {
        var ba = new Array(n);
        var i = s.length - 1;
        var pad;

        if (n < s.length + 11) { throw new Error("Message too long for RSA (n=" + n + ", l=" + s.length + ")"); }

        while (i >= 0 && n > 0)
        {
            var c = s.charCodeAt(i--);

            if (c < 128)
            {
                ba[--n] = c;
            }
            else if ((c > 127) && (c < 2048))
            {
                ba[--n] = (c & 63) | 128;
                ba[--n] = (c >> 6) | 192;
            }
            else
            {
                ba[--n] = (c & 63) | 128;
                ba[--n] = ((c >> 6) & 63) | 128;
                ba[--n] = (c >> 12) | 224;
            }
        }
        ba[--n] = 0;

        // the pad bytes must all be non-zero, because the first zero after the header
        // is what marks where the payload starts. drawn as one block, with any zeros
        // redrawn individually so each byte stays uniform over 1..255
        var pad_block = random_bytes(n > 2 ? n - 2 : 0);
        var pad_index = 0;
        while (n > 2)
        {
            pad = pad_block[pad_index++];
            while (pad == 0) { pad = random_bytes(1)[0]; }
            ba[--n] = pad;
        }
        ba[--n] = 2;
        ba[--n] = 0;

        var hex = "";
        for (i = 0; i < ba.length; i++)
        {
            hex += hex_chars.charAt((ba[i] >> 4) & 15) + hex_chars.charAt(ba[i] & 15);
        }
        return BigInt("0x" + hex);
    }

    /**
     * @brief the inverse of pkcs1_pad: checks the marker and the padding and decodes the utf8 text
     *
     * @param bigint value -> the decrypted block
     * @param number n -> the block length in bytes
     *
     * @return string|null the text, null when the padding does not check out
     */
    function pkcs1_unpad(value, n)
    {
        var b = bigint_to_bytes(value);
        var i = 0;
        var ret = "";

        while (i < b.length && b[i] == 0) { ++i; }
        if (b.length - i != n - 1 || b[i] != 2) { return null; }

        ++i;
        while (b[i] != 0)
        {
            if (++i >= b.length) { return null; }
        }

        while (++i < b.length)
        {
            var c = b[i] & 255;

            if (c < 128)
            {
                ret += String.fromCharCode(c);
            }
            else if ((c > 191) && (c < 224))
            {
                ret += String.fromCharCode(((c & 31) << 6) | (b[i + 1] & 63));
                ++i;
            }
            else
            {
                ret += String.fromCharCode(((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63));
                i += 2;
            }
        }
        return ret;
    }

    // the key object: native BigInt fields under the names the old RSAKey used, so anything
    // holding a key keeps working

    /**
     * @brief an rsa key: n and e, and for a private key d, p, q, dmp1, dmq1 and coeff, all BigInts
     *
     * @return void a constructor, used with new
     */
    function RSAKey()
    {
        this.n = null;
        this.e = 0;
        this.d = null;
        this.p = null;
        this.q = null;
        this.dmp1 = null;
        this.dmq1 = null;
        this.coeff = null;
    }

    /**
     * @brief loads the public half from hex; throws when either part is missing
     *
     * @param string N -> the modulus in hex
     * @param string E -> the exponent in hex
     *
     * @return void
     */
    RSAKey.prototype.setPublic = function (N, E)
    {
        if (N == null || E == null || N.length == 0 || E.length == 0) { throw new Error("Invalid RSA public key"); }
        this.n = bigint_from_hex(N);
        this.e = BigInt(parseInt(E, 16));
    };

    /**
     * @brief loads a whole private key from hex; throws when the public part is missing
     *
     * @param string N -> the modulus
     * @param string E -> the public exponent
     * @param string D -> the private exponent
     * @param string P -> the first prime
     * @param string Q -> the second prime
     * @param string DP -> d mod (p-1)
     * @param string DQ -> d mod (q-1)
     * @param string C -> the crt coefficient
     *
     * @return void
     */
    RSAKey.prototype.setPrivateEx = function (N, E, D, P, Q, DP, DQ, C)
    {
        if (N == null || E == null || N.length == 0 || E.length == 0) { throw new Error("Invalid RSA private key"); }
        this.n = bigint_from_hex(N);
        this.e = BigInt(parseInt(E, 16));
        this.d = bigint_from_hex(D);
        this.p = bigint_from_hex(P);
        this.q = bigint_from_hex(Q);
        this.dmp1 = bigint_from_hex(DP);
        this.dmq1 = bigint_from_hex(DQ);
        this.coeff = bigint_from_hex(C);
    };

    /**
     * @brief x^e mod n, on the wasm
     *
     * @param bigint x -> the block
     *
     * @return bigint the result
     */
    RSAKey.prototype.doPublic = function (x)
    {
        return modpow_wasm(x, this.e, this.n);
    };

    /**
     * @brief x^d mod n, by the crt combination rsa.js did when p and q are known, both exponentiations on the wasm
     *
     * @param bigint x -> the block
     *
     * @return bigint the result
     */
    RSAKey.prototype.doPrivate = function (x)
    {
        if (this.p == null || this.q == null)
        {
            return modpow_wasm(x, this.d, this.n);
        }

        // the same crt combine rsa.js did, with both exponentiations on the wasm
        var xp = modpow_wasm(x % this.p, this.dmp1, this.p);
        var xq = modpow_wasm(x % this.q, this.dmq1, this.q);

        while (xp < xq) { xp = xp + this.p; }
        return (((xp - xq) * this.coeff) % this.p) * this.q + xq;
    };

    /**
     * @brief pkcs#1 encryption of a text with the public half
     *
     * @param string text -> the plaintext
     *
     * @return string the ciphertext as an even-length hex string
     */
    RSAKey.prototype.encrypt = function (text)
    {
        var m = pkcs1_pad(text, (bit_length(this.n) + 7) >> 3);
        var c = this.doPublic(m);
        var h = c.toString(16);

        if ((h.length & 1) == 0) { return h; }
        return "0" + h;
    };

    /**
     * @brief undoes RSAKey.prototype.encrypt with the private half
     *
     * @param string ctext -> the ciphertext in hex
     *
     * @return string|null the plaintext, null when the padding does not check out
     */
    RSAKey.prototype.decrypt = function (ctext)
    {
        var c;

        try { c = bigint_from_hex(ctext); }
        catch (err) { return null; }

        var m = this.doPrivate(c);
        return pkcs1_unpad(m, (bit_length(this.n) + 7) >> 3);
    };

    my.RSAKey = RSAKey;

    // -----------------------------------------------------------------------
    // aes-cbc over aes-js, in cryptico's layout: the iv is block 0 of the output
    // -----------------------------------------------------------------------

    /**
     * @brief aes-cbc under a random iv, the iv prepended, as base64
     *
     * @param string plaintext -> the text
     * @param array key -> the aes key bytes
     *
     * @return string the base64 ciphertext
     */
    my.encryptAESCBC = function (plaintext, key)
    {
        var blocks = my.pad16(my.string2bytes(plaintext));
        var iv = my.blockIV();
        var cbc = new aesjs.ModeOfOperation.cbc(key, iv);
        var encrypted = Array.prototype.slice.call(cbc.encrypt(blocks));

        return my.b256to64(my.bytes2string(iv.concat(encrypted)));
    };

    /**
     * @brief the inverse of encryptAESCBC; a partial trailing block is cut, since aes-js rejects it
     *
     * @param string encrypted_text -> the base64 ciphertext, iv first
     * @param array key -> the aes key bytes
     *
     * @return string the plaintext, "" when the input is too short
     */
    my.decryptAESCBC = function (encrypted_text, key)
    {
        var bytes = my.string2bytes(my.b64to256(encrypted_text));
        var iv = bytes.slice(0, 16);
        var body = bytes.slice(16);

        // aes-js rejects a partial trailing block; the old code fed it garbage instead
        body = body.slice(0, body.length - (body.length % 16));
        if (iv.length < 16 || body.length == 0) { return ""; }

        var cbc = new aesjs.ModeOfOperation.cbc(key, iv);
        var decrypted = Array.prototype.slice.call(cbc.decrypt(body));

        return my.bytes2string(my.depad(decrypted));
    };

    // -----------------------------------------------------------------------
    // keys and the hybrid envelope
    // -----------------------------------------------------------------------

    /**
     * @brief the identity keypair: the seeded prime walk in the wasm, which reproduces the old jsbn walk bit for bit (see client-webassemblies/rsa-keygen)
     *
     * @param string passphrase -> the seed, hashed with sha256
     * @param number bitlength -> the modulus size
     *
     * @return RSAKey the keypair
     */
    my.generateRSAKey = function (passphrase, bitlength)
    {
        var wasm = rsa_crypto__get_rsa_keygen_wasm();
        var seed_hex = sha256.hex(passphrase);
        var memory_bytes = new Uint8Array(wasm.memory.buffer);
        var seed_ptr = wasm.rsa_keygen__get_seed_buffer();
        var i, f, b;

        for (i = 0; i < seed_hex.length; i++)
        {
            memory_bytes[seed_ptr + i] = seed_hex.charCodeAt(i);
        }

        if (wasm.rsa_keygen__generate(bitlength, seed_hex.length) != 1)
        {
            throw new Error("rsa_keygen__generate rejected bits=" + bitlength);
        }

        // results are minimal hex in the order n, e, d, p, q, dmp1, dmq1, coeff
        memory_bytes = new Uint8Array(wasm.memory.buffer);
        var fields = [];
        for (f = 0; f < 8; f++)
        {
            var result_ptr = wasm.rsa_keygen__get_result(f);
            var hex = "";
            for (b = result_ptr; memory_bytes[b] != 0; b++)
            {
                hex += String.fromCharCode(memory_bytes[b]);
            }
            fields.push(hex);
        }

        var key = new RSAKey();
        key.setPrivateEx(fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]);
        return key;
    };

    /**
     * @brief the string form of a public key: the modulus as base64
     *
     * @param RSAKey rsakey -> the key
     *
     * @return string the public key string
     */
    my.publicKeyString = function (rsakey)
    {
        return my.b16to64(rsakey.n.toString(16));
    };

    /**
     * @brief a public key from its string form (the modulus in base64, anything after a "|" ignored), exponent 3
     *
     * @param string string -> the public key string
     *
     * @return RSAKey the key
     */
    my.publicKeyFromString = function (string)
    {
        var N = my.b64to16(string.split("|")[0]);
        var rsa = new RSAKey();

        rsa.setPublic(N, "03");
        return rsa;
    };

    /**
     * @brief hybrid encryption for a receiver: a fresh aes key rsa-encrypted with their public key, "?", then the aes-cbc ciphertext
     *
     * @param string plaintext -> the text
     * @param string publickeystring -> the receiver's public key string
     *
     * @return object { status: "success", cipher } or { status: "Invalid public key" }
     */
    my.encrypt = function (plaintext, publickeystring)
    {
        var cipherblock = "";
        var aeskey = my.generateAESKey();

        try
        {
            var publickey = my.publicKeyFromString(publickeystring);
            cipherblock += my.b16to64(publickey.encrypt(my.bytes2string(aeskey))) + "?";
        }
        catch (err)
        {
            return { status: "Invalid public key" };
        }

        cipherblock += my.encryptAESCBC(plaintext, aeskey);
        return { status: "success", cipher: cipherblock };
    };

    /**
     * @brief the inverse of lemon_crypto.encrypt with our private key
     *        lemon-chat never signs; a signed payload from another cryptico client still yields its
     *        plaintext, it just is not verified here
     *
     * @param string ciphertext -> the hybrid ciphertext
     * @param RSAKey key -> our private key
     *
     * @return object { status: "success", plaintext, signature } or { status: "failure" } when the aes key does not decrypt
     */
    my.decrypt = function (ciphertext, key)
    {
        var cipherblock = ciphertext.split("?");
        var aeskey = key.decrypt(my.b64to16(cipherblock[0]));

        if (aeskey == null) { return { status: "failure" }; }

        aeskey = my.string2bytes(aeskey);

        // lemon-chat never signs; a signed payload from another cryptico client still
        // yields its plaintext, it just is not verified here
        var plaintext = my.decryptAESCBC(cipherblock[1], aeskey).split(signature_separator);

        return {
            status: "success",
            plaintext: plaintext[0],
            signature: (plaintext.length == 3) ? "unverified" : "unsigned"
        };
    };

    /**
     * @brief the server's rsa challenge: pkcs#1 ciphertext, base64, no aes layer
     *
     * @param string ciphertext -> the challenge in base64
     * @param RSAKey key -> our private key
     *
     * @return string|null the plaintext, null when the padding does not check out
     */
    my.challenge_decrypt = function (ciphertext, key)
    {
        return key.decrypt(my.b64to16(ciphertext));
    };

    return my;

}());
