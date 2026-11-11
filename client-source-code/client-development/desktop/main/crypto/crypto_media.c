// Existing version 2 media framing; channel ciphertext stays inside session protection.
#include "crypto/crypto_media.h"
#include "clib.h"
#include "../../third-party/kokke-tiny-aes-c/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"

// declarations
static void _crypto_media_internal__u64(ubyte *bytes, uint64 value);
static uint64 _crypto_media_internal__read_u64(const ubyte *bytes);
static void _crypto_media_internal__crypt(const ubyte *key, const ubyte *block, ubyte *bytes, uint64 size);
static boole _crypto_media_internal__mac(const ubyte *key, const ubyte *bytes, uint64 size, ubyte *output);

/**
 * @brief writes the media protocol's eight-byte big-endian counter.
 * @param ubyte* bytes -> destination with eight bytes
 * @param uint64 value -> counter to serialize
 * @return void
 */
static void _crypto_media_internal__u64(ubyte *bytes, uint64 value)
{
    int64 index = 0;

    // write eight bytes
    for (index = 0; index < 8; index++)
    {
        bytes[index] = (ubyte)(value >> (56 - index * 8));
    }
}

/**
 * @brief reads an existing frame counter without alignment assumptions.
 * @param const ubyte* bytes -> eight readable bytes
 * @return uint64 -> decoded counter
 */
static uint64 _crypto_media_internal__read_u64(const ubyte *bytes)
{
    uint64 value = 0;
    int64 index = 0;

    // read eight bytes
    for (index = 0; index < 8; index++)
    {
        value = (value << 8) | bytes[index];
    }
    return value;
}

/**
 * @brief applies AES-256 CTR in place using an explicit initial block.
 * @param const ubyte* key -> 32-byte key
 * @param const ubyte* block -> 16-byte initial counter
 * @param ubyte* bytes -> writable payload
 * @param uint64 size -> payload length
 * @return void
 */
static void _crypto_media_internal__crypt(const ubyte *key, const ubyte *block, ubyte *bytes, uint64 size)
{
    struct AES_ctx context = { 0 };

    // apply the cipher
    AES_init_ctx_iv(&context, key, block);
    AES_CTR_xcrypt_buffer(&context, bytes, size);

    // clear cipher state
    mbedtls_platform_zeroize(&context, sizeof(context));
}

/**
 * @brief calculates the existing HMAC-SHA256 authentication code.
 * @param const ubyte* key -> 32-byte authentication key
 * @param const ubyte* bytes -> readable authenticated bytes
 * @param uint64 size -> byte count
 * @param ubyte* output -> 32-byte destination
 * @return boole -> 1 when calculated
 */
static boole _crypto_media_internal__mac(const ubyte *key, const ubyte *bytes, uint64 size, ubyte *output)
{
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, bytes, size, output) == 0;
}

/**
 * @brief parses an exact-size hex key or ticket, rejecting partial and oversized input.
 * @param const char* text -> NUL-terminated hex text
 * @param ubyte* output -> destination with size bytes
 * @param uint64 size -> expected decoded length
 * @return boole -> 1 on success, otherwise output is cleared
 */
boole crypto_media__hex(const char *text, ubyte *output, uint64 size)
{
    uint64 index = 0;
    int64 value = 0;
    ubyte character = 0;

    // check arguments
    if (!text || !output || size > 64)
    {
        return 0;
    }

    // clear the output
    clib__null_memory(output, size);

    // check text length
    if (clib__strlen(text) != size * 2)
    {
        return 0;
    }

    // convert hex text
    for (index = 0; index < size * 2; index++)
    {
        // read one hex digit
        character = (ubyte)text[index];
        if (character >= '0' && character <= '9')
        {
            value = character - '0';
        }
        else if (character >= 'a' && character <= 'f')
        {
            value = character - 'a' + 10;
        }
        else if (character >= 'A' && character <= 'F')
        {
            value = character - 'A' + 10;
        }
        else
        {
            clib__null_memory(output, size);
            return 0;
        }

        // add digit to byte
        output[index / 2] = (ubyte)((output[index / 2] << 4) | value);
    }
    return 1;
}

/**
 * @brief constructs the existing one-use audio WebSocket authentication frame.
 * @param const ubyte key -> 32-byte ticket authentication key
 * @param const ubyte ticket -> 16-byte session ticket
 * @param uint id -> authenticated server client ID
 * @param ubyte output -> 53-byte frame destination
 * @return boole -> 1 when complete
 */
boole crypto_media__proof(const ubyte key[32], const ubyte ticket[16], uint id, ubyte output[53])
{
    // write type, id, ticket
    clib__null_memory(output, 53);
    output[0] = 1;
    output[1] = (ubyte)(id >> 24);
    output[2] = (ubyte)(id >> 16);
    output[3] = (ubyte)(id >> 8);
    output[4] = (ubyte)id;
    clib__memcpy(output + 5, ticket, 16);

    // add the HMAC
    return _crypto_media_internal__mac(key, output, 21, output + 21);
}

/**
 * @brief verifies the server's audio attachment acknowledgement in constant time.
 * @param const ubyte key -> 32-byte ticket authentication key
 * @param const ubyte ticket -> expected 16-byte ticket
 * @param const ubyte* bytes -> received frame
 * @param uint64 size -> received byte count
 * @return boole -> 1 only for an authentic acknowledgement of this ticket
 */
boole crypto_media__acknowledgement(const ubyte key[32], const ubyte ticket[16], const ubyte *bytes, uint64 size)
{
    ubyte code[32] = { 0 };
    boole valid = 0;

    // check ticket and HMAC
    if (bytes && size == 49 && bytes[0] == 2 && _crypto_media_internal__mac(key, bytes, 17, code))
    {
        valid = (mbedtls_ct_memcmp(ticket, bytes + 1, 16) | mbedtls_ct_memcmp(code, bytes + 17, 32)) == 0;
    }

    // clear HMAC buffer
    mbedtls_platform_zeroize(code, sizeof(code));
    return valid;
}

/**
 * @brief applies one inner AES layer using the sender ID, sender counter and optional channel IV.
 * @param const ubyte key -> 32-byte key
 * @param const ubyte* iv -> 16-byte channel IV, or NULL for server/bot keys
 * @param uint sender -> server sender ID
 * @param uint64 counter -> sender's frame counter
 * @param ubyte* data -> writable payload
 * @param uint64 size -> payload byte count
 * @return void
 */
void crypto_media__layer(const ubyte key[32], const ubyte *iv, uint sender, uint64 counter, ubyte *data, uint64 size)
{
    ubyte block[16] = { 0 };
    int64 index = 0;

    // build counter block
    for (index = 0; index < 4; index++)
    {
        block[index] = (ubyte)(sender >> (index * 8));
    }
    _crypto_media_internal__u64(block + 4, counter);

    // XOR with channel IV
    if (iv)
    {
        for (index = 0; index < 16; index++)
        {
            block[index] ^= iv[index];
        }
    }

    // apply the cipher
    _crypto_media_internal__crypt(key, block, data, size);
}

/**
 * @brief wraps channel ciphertext in the existing server-key and per-session media layers.
 * @param const desktop_media_keys_t* keys -> worker-owned session keys
 * @param uint sender -> local authenticated client ID
 * @param uint64 counter -> unique counter for this packet; never reuse under these keys
 * @param const ubyte* content -> channel-encrypted sequence and Opus packet
 * @param uint64 size -> content byte count
 * @param ubyte* output -> frame destination
 * @param uint64 capacity -> actual destination capacity
 * @return uint64 -> frame length, or zero on failure
 */
uint64 crypto_media__seal(const desktop_media_keys_t *keys, uint sender, uint64 counter, const ubyte *content, uint64 size, ubyte *output, uint64 capacity)
{
    ubyte block[16] = { 0 };
    ubyte code[32] = { 0 };
    uint64 index = 0;

    // check arguments
    if (!keys || !content || !output || size < 3 || size > DESKTOP_MEDIA_CONTENT || capacity < size + DESKTOP_MEDIA_OVERHEAD || keys->server_count > 8)
    {
        return 0;
    }

    // write version and counter
    clib__null_memory(output, size + DESKTOP_MEDIA_OVERHEAD);
    output[0] = 2;
    _crypto_media_internal__u64(output + 1, counter);

    // write sender and content
    for (index = 0; index < 4; index++)
    {
        output[9 + index] = (ubyte)(sender >> (index * 8));
    }
    _crypto_media_internal__u64(output + 13, counter);
    clib__memcpy(output + 21, content, size);

    // encrypt with server keys
    for (index = 0; index < keys->server_count; index++)
    {
        // build counter block
        clib__null_memory(block, sizeof(block));
        for (unsigned int byte = 0; byte < 4; byte++)
        {
            block[byte] = (ubyte)(sender >> (byte * 8));
        }
        _crypto_media_internal__u64(block + 4, counter);

        // encrypt the content
        if (!ciphers__crypt(keys->server_types[index] ? keys->server_types[index] : LEMON_AES256, keys->server[index], block, output + 21, size))
        {
            return 0;
        }
    }

    // encrypt with session keys
    clib__null_memory(block, sizeof(block));
    _crypto_media_internal__u64(block, counter);
    if (keys->negotiated.count)
    {
        if (!keys_session__crypt(&keys->negotiated, 0, 1, block, output + 9, size + 12))
        {
            return 0;
        }
    }
    else
    {
        _crypto_media_internal__crypt(keys->encryption, block, output + 9, size + 12);
    }

    // add the HMAC
    if (!_crypto_media_internal__mac(keys->authentication, output, size + 21, code))
    {
        clib__null_memory(output, size + DESKTOP_MEDIA_OVERHEAD);
        return 0;
    }
    clib__memcpy(output + size + 21, code, 8);
    mbedtls_platform_zeroize(code, sizeof(code));
    return size + DESKTOP_MEDIA_OVERHEAD;
}

/**
 * @brief authenticates an incoming media frame before removing session and server layers.
 * @param const desktop_media_keys_t* keys -> worker-owned session keys
 * @param const ubyte* frame -> received version 2 frame
 * @param uint64 size -> complete frame length
 * @param uint* sender -> receives authenticated sender ID
 * @param uint64* sender_counter -> receives the original sender's counter
 * @param uint64* frame_counter -> receives server counter for replay checks
 * @param ubyte* content -> receives channel ciphertext
 * @param uint64 capacity -> destination capacity
 * @return uint64 -> content length, or zero for invalid input
 */
uint64 crypto_media__open(const desktop_media_keys_t *keys, const ubyte *frame, uint64 size, uint *sender, uint64 *sender_counter, uint64 *frame_counter, ubyte *content, uint64 capacity)
{
    ubyte body[DESKTOP_MEDIA_CONTENT + 12] = { 0 };
    ubyte block[16] = { 0 };
    ubyte code[32] = { 0 };
    uint64 length = 0;
    uint64 index = 0;

    // check arguments
    if (!keys || !frame || !sender || !sender_counter || !frame_counter || !content || keys->server_count > 8 || size < DESKTOP_MEDIA_OVERHEAD + 3 || size > DESKTOP_MEDIA_FRAME || frame[0] != 2)
    {
        return 0;
    }

    // check size and HMAC
    length = size - DESKTOP_MEDIA_OVERHEAD;
    if (capacity < length || !_crypto_media_internal__mac(keys->negotiated.count ? keys->negotiated.server_mac : keys->authentication, frame, size - 8, code) || mbedtls_ct_memcmp(code, frame + size - 8, 8) != 0)
    {
        mbedtls_platform_zeroize(code, sizeof(code));
        return 0;
    }

    // read frame counter
    *frame_counter = _crypto_media_internal__read_u64(frame + 1);
    _crypto_media_internal__u64(block, *frame_counter);
    block[8] = 1;

    // decrypt with session keys
    clib__memcpy(body, frame + 9, length + 12);
    if (keys->negotiated.count)
    {
        if (!keys_session__crypt(&keys->negotiated, 1, 0, block, body, length + 12))
        {
            return 0;
        }
    }
    else
    {
        _crypto_media_internal__crypt(keys->encryption, block, body, length + 12);
    }

    // read sender and counter
    *sender = (uint)body[0] | ((uint)body[1] << 8) | ((uint)body[2] << 16) | ((uint)body[3] << 24);
    *sender_counter = _crypto_media_internal__read_u64(body + 4);

    // decrypt with server keys
    for (index = keys->server_count; index > 0; index--)
    {
        // build counter block
        clib__null_memory(block, sizeof(block));
        for (unsigned int byte = 0; byte < 4; byte++)
        {
            block[byte] = (ubyte)(*sender >> (byte * 8));
        }
        _crypto_media_internal__u64(block + 4, *sender_counter);

        // decrypt the content
        if (!ciphers__crypt(keys->server_types[index - 1] ? keys->server_types[index - 1] : LEMON_AES256, keys->server[index - 1], block, body + 12, length))
        {
            ciphers__clear(body, sizeof(body));
            return 0;
        }
    }

    // copy the content
    clib__memcpy(content, body + 12, length);

    // clear the buffers
    mbedtls_platform_zeroize(body, sizeof(body));
    mbedtls_platform_zeroize(code, sizeof(code));
    return length;
}
