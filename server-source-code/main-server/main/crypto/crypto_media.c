// authenticates audio and video packets and applies their connection encryption
// forwarding preserves the encrypted channel content and verifies the sender id
//
// packet layout:
//   1 byte: version 2
//   8 bytes: connection counter, most significant byte first
//   variable length: encrypted body
//   8 bytes: truncated HMAC SHA256 of the header and encrypted body
//
// the body starts with the sender id in 4 bytes, least significant byte first
// its next 8 bytes hold the sender counter, most significant byte first
// the remaining bytes contain encrypted audio or video
//
// the connection IV contains the counter, direction, stream and six zero bytes
// the inner IV contains the sender id, sender counter and four zero bytes
// when a layer has its own IV, it is XORed with the inner IV
#include "../definitions.h"
#include "ciphers.h"
#include "crypto_metadata.h"

#include "../../third-party/dave-g-json/cJSON.h"
#include "../base.h" // server settings
#include "../../third-party/rxi-log/log.h"

#include "../clib/clib.h"

#include "crypto_media.h"

#include <string.h>

#define MEDIA_DIRECTION_FROM_CLIENT 0
#define MEDIA_DIRECTION_TO_CLIENT 1

// declarations
static void _crypto_media_internal__write_u64(ubyte *out, uint64 value);
static uint64 _crypto_media_internal__read_u64(const ubyte *in);
static void _crypto_media_internal__session_block(uint64 frame_counter, int64 direction, int64 stream, ubyte *out_block);

/**
 * @brief writes a 64 bit number into eight bytes, with the most significant byte first
 *
 * @param ubyte* out -> buffer with room for eight bytes
 * @param uint64 value -> number to write
 *
 * @return void
 */
static void _crypto_media_internal__write_u64(ubyte *out, uint64 value)
{
    uint64 i = 0;

    // write highest byte first
    for (i = 0; i < 8; i++)
    {
        out[i] = (ubyte)(value >> (56 - i * 8));
    }
}

/**
 * @brief reads eight bytes as a 64 bit number, with the most significant byte first
 *
 * @param const ubyte* in -> buffer containing at least eight bytes
 *
 * @return uint64 -> number stored in those eight bytes
 */
static uint64 _crypto_media_internal__read_u64(const ubyte *in)
{
    uint64 value = 0;
    uint64 i = 0;

    // read highest byte first
    for (i = 0; i < 8; i++)
    {
        value = (value << 8) | in[i];
    }
    return value;
}

/**
 * @brief builds the 16 byte AES starting value for one audio or video packet on a connection
 *
 * @param uint64 frame_counter -> connection packet number written into the first eight bytes
 * @param int64 direction -> MEDIA_DIRECTION_FROM_CLIENT or MEDIA_DIRECTION_TO_CLIENT
 * @param int64 stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param ubyte* out_block -> caller owned buffer with room for the 16 byte starting value
 *
 * @return void
 */
static void _crypto_media_internal__session_block(uint64 frame_counter, int64 direction, int64 stream, ubyte *out_block)
{
    // clear the block
    clib__null_memory(out_block, 16);

    // write counter, direction, stream
    _crypto_media_internal__write_u64(out_block, frame_counter);
    out_block[8] = (ubyte)direction;
    out_block[9] = (ubyte)stream;
}

/**
 * @brief builds the starting value used to encrypt the audio or video contents inside a packet
 *
 * @param uint64 sender_id -> sender ID. Its low 32 bits are written into the starting value
 * @param uint64 sender_counter -> packet number assigned by the original sender
 * @param const ubyte* layer_iv -> optional 16 byte starting value belonging to the key, or NULL_POINTER
 * @param ubyte* out_block -> caller owned buffer with room for the 16 byte result
 *
 * @return void
 */
void crypto_media__inner_block(uint64 sender_id, uint64 sender_counter, const ubyte *layer_iv, ubyte *out_block)
{
    uint64 i = 0;

    // clear the block
    clib__null_memory(out_block, 16);

    // write sender and counter
    out_block[0] = (ubyte)sender_id;
    out_block[1] = (ubyte)(sender_id >> 8);
    out_block[2] = (ubyte)(sender_id >> 16);
    out_block[3] = (ubyte)(sender_id >> 24);
    _crypto_media_internal__write_u64(out_block + 4, sender_counter);

    // combine the channel key's IV when present
    if (layer_iv != NULL_POINTER)
    {
        for (i = 0; i < 16; i++)
        {
            out_block[i] ^= layer_iv[i];
        }
    }
}

/**
 * @brief applies one key to the audio or video contents using the sender ID and packet number
 *
 * @param const crypto_key_t* key -> encryption key for this layer
 * @param const ubyte* layer_iv -> optional 16 byte starting value for the key, or NULL_POINTER
 * @param uint64 sender_id -> ID of the client or music bot that produced the packet
 * @param uint64 sender_counter -> packet number assigned by that sender
 * @param ubyte* data -> caller owned contents changed in place
 * @param uint64 size -> number of bytes in data
 *
 * @return boole -> TRUE when the encryption method accepted the key and processed the data. Otherwise FALSE
 */
boole crypto_media__apply_inner_layer(const crypto_key_t *key, const ubyte *layer_iv, uint64 sender_id, uint64 sender_counter, ubyte *data, uint64 size)
{
    ubyte block[16] = { 0 };

    crypto_media__inner_block(sender_id, sender_counter, layer_iv, block);
    return crypto_metadata__crypt(key, block, sizeof(block), data, size, CRYPTO_ENCRYPT);
}

/**
 * @brief writes the sender ID and original packet number before the encrypted audio or video
 *        contents
 *
 * @param ubyte* body -> buffer with room for the 12 byte body header
 * @param uint64 sender_id -> sender ID. Its low 32 bits are written with the least significant byte first
 * @param uint64 sender_counter -> sender packet number, written with the most significant byte first
 *
 * @return void
 */
// writes the first 12 bytes of a body that the server builds itself (the music bot frames)
/**
 * @brief writes the sender and sequence fields of a media body
 *
 * @param ubyte * body -> writable body with at least 12 bytes
 * @param uint64 sender_id -> sender ID encoded in the first four bytes
 * @param uint64 sender_counter -> 64 bit sequence value
 *
 * @return void
 */
void crypto_media__write_body_header(ubyte *body, uint64 sender_id, uint64 sender_counter)
{
    // write the sender ID
    body[0] = (ubyte)sender_id;
    body[1] = (ubyte)(sender_id >> 8);
    body[2] = (ubyte)(sender_id >> 16);
    body[3] = (ubyte)(sender_id >> 24);

    // write the sender counter
    _crypto_media_internal__write_u64(body + 4, sender_counter);
}

/**
 * @brief checks a received packet and removes its connection encryption while keeping its channel
 *        encryption intact
 *
 * @param client_t* sender -> client whose keys protect the packet and whose ID must appear in its body
 * @param int64 stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param const ubyte* frame -> received packet containing its header, encrypted body, and check value
 * @param uint64 size -> number of bytes in frame
 * @param ubyte* out_body -> caller owned buffer that receives the sender header and still encrypted channel contents
 * @param uint64 out_capacity -> size of out_body in bytes
 *
 * @attention the caller must keep the sender and its keys stable, normally with the clients lock. Ignore out_body when zero is returned. It may contain a partial result
 *
 * @return uint64 -> number of body bytes written, or zero when the packet is invalid, cannot fit, or cannot be decrypted
 */
uint64 crypto_media__decrypt_from_client(client_t *sender, int64 stream, const ubyte *frame, uint64 size, ubyte *out_body, uint64 out_capacity)
{
    crypto_key_t receive_authentication = { 0 };
    ubyte code[32] = { 0 };
    ubyte block[16] = { 0 };
    uint64 body_size = 0;
    uint64 sender_id_in_body = 0;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;

    // check keys, size, version
    status0 = sender->session_keys.is_ready == FALSE;
    status1 = size <= MEDIA_FRAME_OVERHEAD;
    if (status0 == FALSE && status1 == FALSE)
    {
        status2 = frame[0] != MEDIA_FRAME_VERSION;
    }
    if (status0 == TRUE || status1 == TRUE || status2 == TRUE)
    {
        DBG_MEDIA_AUTH log_info("%s %llu %s", "frame of client", sender->client_id, "refused: the client has no keys yet, or the frame is too short, or its version byte is wrong");
        return 0;
    }

    // check the buffer size
    body_size = size - MEDIA_FRAME_HEADER - MEDIA_FRAME_CODE;
    if (body_size > out_capacity)
    {
        return 0;
    }

    // the authentication code is checked first: a frame that fails the check is not decrypted at all
    receive_authentication = sender->session_keys.authentication;
    if (sender->session_keys.negotiated.count != 0)
    {
        crypto_metadata__key_init(&receive_authentication, CRYPTO_KEY_HMAC_SHA256, sender->session_keys.negotiated.client_mac, 32);
    }
    status0 = crypto_metadata__hmac_sha256(&receive_authentication, frame, size - MEDIA_FRAME_CODE, NULL_POINTER, 0, code) == FALSE;
    ciphers__clear(&receive_authentication, sizeof(receive_authentication));
    if (status0 == FALSE)
    {
        status0 = crypto_metadata__equal(code, frame + size - MEDIA_FRAME_CODE, MEDIA_FRAME_CODE) == FALSE;
    }
    if (status0 == TRUE)
    {
        DBG_MEDIA_AUTH log_info("%s %llu %s", "frame of client", sender->client_id, "refused: the authentication code does not match, the frame was changed or made up");
        return 0;
    }

    // remove the connection encryption
    clib__memcpy(out_body, frame + MEDIA_FRAME_HEADER, body_size);
    _crypto_media_internal__session_block(_crypto_media_internal__read_u64(frame + 1), MEDIA_DIRECTION_FROM_CLIENT, stream, block);
    if (!crypto_metadata__crypt_session(&sender->session_keys, block, sizeof(block), out_body, body_size, CRYPTO_DECRYPT))
    {
        return 0;
    }

    // require the sender id to match this connection
    sender_id_in_body = (uint64)out_body[0] | ((uint64)out_body[1] << 8) | ((uint64)out_body[2] << 16) | ((uint64)out_body[3] << 24);
    if (sender_id_in_body != sender->client_id)
    {
        DBG_MEDIA_AUTH log_info("%s %llu %s %llu", "frame of client", sender->client_id, "refused: it names sender", sender_id_in_body);
        return 0;
    }

    // decrypt with password keys
    crypto_media__inner_block(sender_id_in_body, _crypto_media_internal__read_u64(out_body + 4), NULL_POINTER, block);
    if (sender->password_keys != NULL_POINTER && !crypto_metadata__crypt_layers(sender->password_keys->keys, sender->password_keys->count, block, sizeof(block), out_body + MEDIA_BODY_HEADER, body_size - MEDIA_BODY_HEADER, CRYPTO_DECRYPT))
    {
        return 0;
    }
    return body_size;
}

/**
 * @brief protects a copy of an audio or video body with the receiving client's connection keys
 *
 * @param client_t* receiver -> client whose keys are used and whose outgoing packet counter is increased
 * @param int64 stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param const ubyte* body -> sender header followed by contents already encrypted for the channel
 * @param uint64 body_size -> number of bytes in body
 * @param ubyte* out_frame -> caller owned buffer that receives the packet header, encrypted body, and check value
 * @param uint64 out_capacity -> size of out_frame in bytes
 *
 * @attention the caller must keep the client and its keys stable, normally with the clients lock. The counter is increased atomically, so several senders can prepare packets for this client
 *
 * @return uint64 -> total packet length in bytes, or zero when keys are unavailable, output cannot fit, or encryption fails
 */
uint64 crypto_media__encrypt_for_client(client_t *receiver, int64 stream, const ubyte *body, uint64 body_size, ubyte *out_frame, uint64 out_capacity)
{
    uint64 sender_id = 0;
    ubyte code[32] = { 0 };
    ubyte block[16] = { 0 };
    uint64 frame_counter = 0;
    uint64 frame_size = 0;
    boole status0 = FALSE;
    boole status1 = FALSE;
    boole status2 = FALSE;

    // check keys and sizes
    status0 = receiver->session_keys.is_ready == FALSE;
    status1 = body_size < MEDIA_BODY_HEADER;

    // check by subtraction before adding framing bytes or narrowing a copy length
    status2 = out_capacity < MEDIA_FRAME_HEADER + MEDIA_FRAME_CODE || body_size > out_capacity - MEDIA_FRAME_HEADER - MEDIA_FRAME_CODE || body_size > SIZE_MAX - MEDIA_FRAME_HEADER - MEDIA_FRAME_CODE || body_size > INT64_MAX;
    if (status0 == TRUE || status1 == TRUE || status2 == TRUE)
    {
        return 0;
    }
    frame_size = MEDIA_FRAME_HEADER + body_size + MEDIA_FRAME_CODE;

    // several threads can send to this client. increase the counter in one atomic
    // operation so they cannot assign the same packet number to different frames
    frame_counter = atomic_fetch_add_explicit(&receiver->media_send_counter, 1, memory_order_relaxed);

    // write header and body
    out_frame[0] = MEDIA_FRAME_VERSION;
    _crypto_media_internal__write_u64(out_frame + 1, frame_counter);
    clib__memcpy(out_frame + MEDIA_FRAME_HEADER, body, body_size);

    // encrypt with password keys
    sender_id = (uint64)body[0] | ((uint64)body[1] << 8) | ((uint64)body[2] << 16) | ((uint64)body[3] << 24);
    crypto_media__inner_block(sender_id, _crypto_media_internal__read_u64(body + 4), NULL_POINTER, block);
    if (receiver->password_keys != NULL_POINTER && !crypto_metadata__crypt_layers(receiver->password_keys->keys, receiver->password_keys->count, block, sizeof(block), out_frame + MEDIA_FRAME_HEADER + MEDIA_BODY_HEADER, body_size - MEDIA_BODY_HEADER, CRYPTO_ENCRYPT))
    {
        return 0;
    }

    // encrypt with session keys
    _crypto_media_internal__session_block(frame_counter, MEDIA_DIRECTION_TO_CLIENT, stream, block);
    if (!crypto_metadata__crypt_session(&receiver->session_keys, block, sizeof(block), out_frame + MEDIA_FRAME_HEADER, body_size, CRYPTO_ENCRYPT))
    {
        return 0;
    }

    // the authentication code covers the header and the encrypted part, so neither can be changed
    if (crypto_metadata__hmac_sha256(&receiver->session_keys.authentication, out_frame, MEDIA_FRAME_HEADER + body_size, NULL_POINTER, 0, code) == FALSE)
    {
        return 0;
    }
    clib__memcpy(out_frame + MEDIA_FRAME_HEADER + body_size, code, MEDIA_FRAME_CODE);
    return frame_size;
}
