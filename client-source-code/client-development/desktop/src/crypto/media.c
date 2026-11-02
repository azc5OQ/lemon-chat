// Existing version 2 media framing; channel ciphertext stays inside session protection.
#include "crypto/media.h"
#include "clib.h"
#include "aes.h"
#include "mbedtls/md.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"

// declarations
static void _media_internal__u64(unsigned char* bytes, uint64 value);
static uint64 _media_internal__read_u64(const unsigned char* bytes);
static void _media_internal__crypt(const unsigned char* key, const unsigned char* block, unsigned char* bytes, uint64 size);
static boole _media_internal__mac(const unsigned char* key, const unsigned char* bytes, uint64 size, unsigned char* output);

/**
 * @brief writes the media protocol's eight-byte big-endian counter.
 * @param unsigned char* bytes -> destination with eight bytes
 * @param uint64 value -> counter to serialize
 * @return void
 */
static void _media_internal__u64(unsigned char* bytes, uint64 value)
{
    int64 index = 0;
    for (index = 0; index < 8; index++)
    {
        bytes[index] = (unsigned char)(value >> (56 - index * 8));
    }
}

/**
 * @brief reads an existing frame counter without alignment assumptions.
 * @param const unsigned char* bytes -> eight readable bytes
 * @return uint64 -> decoded counter
 */
static uint64 _media_internal__read_u64(const unsigned char* bytes)
{
    uint64 value = 0;
    int64 index = 0;
    for (index = 0; index < 8; index++)
    {
        value = (value << 8) | bytes[index];
    }
    return value;
}

/**
 * @brief applies AES-256 CTR in place using an explicit initial block.
 * @param const unsigned char* key -> 32-byte key
 * @param const unsigned char* block -> 16-byte initial counter
 * @param unsigned char* bytes -> writable payload
 * @param uint64 size -> payload length
 * @return void
 */
static void _media_internal__crypt(const unsigned char* key, const unsigned char* block, unsigned char* bytes, uint64 size)
{
    struct AES_ctx context = {0};
    AES_init_ctx_iv(&context, key, block);
    AES_CTR_xcrypt_buffer(&context, bytes, size);
    mbedtls_platform_zeroize(&context, sizeof(context));
}

/**
 * @brief calculates the existing HMAC-SHA256 authentication code.
 * @param const unsigned char* key -> 32-byte authentication key
 * @param const unsigned char* bytes -> readable authenticated bytes
 * @param uint64 size -> byte count
 * @param unsigned char* output -> 32-byte destination
 * @return boole -> 1 when calculated
 */
static boole _media_internal__mac(const unsigned char* key, const unsigned char* bytes, uint64 size, unsigned char* output)
{
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, bytes, size, output) == 0;
}

/**
 * @brief parses an exact-size hex key or ticket, rejecting partial and oversized input.
 * @param const char* text -> NUL-terminated hex text
 * @param unsigned char* output -> destination with size bytes
 * @param uint64 size -> expected decoded length
 * @return boole -> 1 on success, otherwise output is cleared
 */
boole desktop_media__hex(const char* text, unsigned char* output, uint64 size)
{
    uint64 index = 0;
    int64 value = 0;
    unsigned char character = 0;
    if (!text || !output || size > 64)
    {
        return 0;
    }
    clib__null_memory(output, size);
    if (clib__strlen(text) != size * 2)
    {
        return 0;
    }
    for (index = 0; index < size * 2; index++)
    {
        character = (unsigned char)text[index];
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
        output[index / 2] = (unsigned char)((output[index / 2] << 4) | value);
    }
    return 1;
}

/**
 * @brief constructs the existing one-use audio WebSocket authentication frame.
 * @param const unsigned char key -> 32-byte ticket authentication key
 * @param const unsigned char ticket -> 16-byte session ticket
 * @param uint id -> authenticated server client ID
 * @param unsigned char output -> 53-byte frame destination
 * @return boole -> 1 when complete
 */
boole desktop_media__proof(const unsigned char key[32], const unsigned char ticket[16], uint id, unsigned char output[53])
{
    clib__null_memory(output, 53);
    output[0] = 1;
    output[1] = (unsigned char)(id >> 24);
    output[2] = (unsigned char)(id >> 16);
    output[3] = (unsigned char)(id >> 8);
    output[4] = (unsigned char)id;
    clib__memcpy(output + 5, ticket, 16);
    return _media_internal__mac(key, output, 21, output + 21);
}

/**
 * @brief verifies the server's audio attachment acknowledgement in constant time.
 * @param const unsigned char key -> 32-byte ticket authentication key
 * @param const unsigned char ticket -> expected 16-byte ticket
 * @param const unsigned char* bytes -> received frame
 * @param uint64 size -> received byte count
 * @return boole -> 1 only for an authentic acknowledgement of this ticket
 */
boole desktop_media__acknowledgement(const unsigned char key[32], const unsigned char ticket[16], const unsigned char* bytes, uint64 size)
{
    unsigned char code[32] = {0};
    boole valid = 0;
    if (bytes && size == 49 && bytes[0] == 2 && _media_internal__mac(key, bytes, 17, code))
    {
        valid = (mbedtls_ct_memcmp(ticket, bytes + 1, 16) | mbedtls_ct_memcmp(code, bytes + 17, 32)) == 0;
    }
    mbedtls_platform_zeroize(code, sizeof(code));
    return valid;
}

/**
 * @brief applies one inner AES layer using the sender ID, sender counter and optional channel IV.
 * @param const unsigned char key -> 32-byte key
 * @param const unsigned char* iv -> 16-byte channel IV, or NULL for server/bot keys
 * @param uint sender -> server sender ID
 * @param uint64 counter -> sender's frame counter
 * @param unsigned char* data -> writable payload
 * @param uint64 size -> payload byte count
 * @return void
 */
void desktop_media__layer(const unsigned char key[32], const unsigned char* iv, uint sender, uint64 counter, unsigned char* data, uint64 size)
{
    unsigned char block[16] = {0};
    int64 index = 0;
    for (index = 0; index < 4; index++)
    {
        block[index] = (unsigned char)(sender >> (index * 8));
    }
    _media_internal__u64(block + 4, counter);
    if (iv)
    {
        for (index = 0; index < 16; index++)
        {
            block[index] ^= iv[index];
        }
    }
    _media_internal__crypt(key, block, data, size);
}

/**
 * @brief wraps channel ciphertext in the existing server-key and per-session media layers.
 * @param const desktop_media_keys_t* keys -> worker-owned session keys
 * @param uint sender -> local authenticated client ID
 * @param uint64 counter -> unique counter for this packet; never reuse under these keys
 * @param const unsigned char* content -> channel-encrypted sequence and Opus packet
 * @param uint64 size -> content byte count
 * @param unsigned char* output -> frame destination
 * @param uint64 capacity -> actual destination capacity
 * @return uint64 -> frame length, or zero on failure
 */
uint64 desktop_media__seal(const desktop_media_keys_t* keys, uint sender, uint64 counter,
    const unsigned char* content, uint64 size, unsigned char* output, uint64 capacity)
{
    unsigned char block[16] = {0};
    unsigned char code[32] = {0};
    uint64 index = 0;
    if (!keys || !content || !output || size < 3 || size > DESKTOP_MEDIA_CONTENT
        || capacity < size + DESKTOP_MEDIA_OVERHEAD || keys->server_count > 8)
    {
        return 0;
    }
    clib__null_memory(output, size + DESKTOP_MEDIA_OVERHEAD);
    output[0] = 2;
    _media_internal__u64(output + 1, counter);
    for (index = 0; index < 4; index++)
    {
        output[9 + index] = (unsigned char)(sender >> (index * 8));
    }
    _media_internal__u64(output + 13, counter);
    clib__memcpy(output + 21, content, size);
    for (index = 0; index < keys->server_count; index++)
    {
        desktop_media__layer(keys->server[index], NULL, sender, counter, output + 21, size);
    }
    _media_internal__u64(block, counter);
    _media_internal__crypt(keys->encryption, block, output + 9, size + 12);
    if (!_media_internal__mac(keys->authentication, output, size + 21, code))
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
 * @param const unsigned char* frame -> received version 2 frame
 * @param uint64 size -> complete frame length
 * @param uint* sender -> receives authenticated sender ID
 * @param uint64* sender_counter -> receives the original sender's counter
 * @param uint64* frame_counter -> receives server counter for replay checks
 * @param unsigned char* content -> receives channel ciphertext
 * @param uint64 capacity -> destination capacity
 * @return uint64 -> content length, or zero for invalid input
 */
uint64 desktop_media__open(const desktop_media_keys_t* keys, const unsigned char* frame, uint64 size,
    uint* sender, uint64* sender_counter, uint64* frame_counter, unsigned char* content, uint64 capacity)
{
    unsigned char body[DESKTOP_MEDIA_CONTENT + 12] = {0};
    unsigned char block[16] = {0};
    unsigned char code[32] = {0};
    uint64 length = 0;
    uint64 index = 0;
    if (!keys || !frame || !sender || !sender_counter || !frame_counter || !content || keys->server_count > 8
        || size < DESKTOP_MEDIA_OVERHEAD + 3 || size > DESKTOP_MEDIA_FRAME || frame[0] != 2)
    {
        return 0;
    }
    length = size - DESKTOP_MEDIA_OVERHEAD;
    if (capacity < length || !_media_internal__mac(keys->authentication, frame, size - 8, code)
        || mbedtls_ct_memcmp(code, frame + size - 8, 8) != 0)
    {
        mbedtls_platform_zeroize(code, sizeof(code));
        return 0;
    }
    *frame_counter = _media_internal__read_u64(frame + 1);
    _media_internal__u64(block, *frame_counter);
    block[8] = 1;
    clib__memcpy(body, frame + 9, length + 12);
    _media_internal__crypt(keys->encryption, block, body, length + 12);
    *sender = (uint)body[0] | ((uint)body[1] << 8) | ((uint)body[2] << 16) | ((uint)body[3] << 24);
    *sender_counter = _media_internal__read_u64(body + 4);
    for (index = keys->server_count; index > 0; index--)
    {
        desktop_media__layer(keys->server[index - 1], NULL, *sender, *sender_counter, body + 12, length);
    }
    clib__memcpy(content, body + 12, length);
    mbedtls_platform_zeroize(body, sizeof(body));
    mbedtls_platform_zeroize(code, sizeof(code));
    return length;
}
