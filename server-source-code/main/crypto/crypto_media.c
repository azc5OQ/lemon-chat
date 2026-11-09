// These functions prepare and check the audio and video packets sent between
// clients and the server. crypto_media.c explains which bytes go into each packet
// and how it checks the sender. The encryption functions it calls are in
// crypto_metadata.c, where metadata messages use those same functions too.
// This file checks audio and video packets that travel between a client and
// the server. When a packet arrives, it checks that the protected bytes have
// not been changed and that the sender ID belongs to the connected client.
// When the server forwards a packet, it protects a separate copy with each
// receiving client's keys. The voice or video inside stays encrypted with the
// channel's keys; the server does not need those keys to forward the packet.
// The encryption and message-checking functions are in crypto_metadata.c.
//
// Each packet is sent in this order:
//   1 byte: the packet format version, currently 2.
//   8 bytes: the number assigned to this packet, highest-value byte first.
//   A variable number of bytes: the contents encrypted for this connection.
//   8 bytes: a check value calculated from the preceding bytes and a secret key.
// The full check value is called HMAC-SHA256 and has 32 bytes; this packet format
// sends its first 8 bytes. A receiver calculates it again and rejects the packet
// if they differ, before trying to decrypt anything.
//
// After decrypting the connection's protection, the contents start with a
// 4-byte sender ID, lowest-value byte first, followed by the sender's 8-byte
// packet number, highest-value byte first. The rest is the audio or video data
// that the sender already encrypted with the keys known to its channel.
//
// AES also needs a 16-byte starting value for each encryption. For protection
// between a client and the server, this contains the 8-byte packet number, one
// direction byte, one audio/video byte, and six zero bytes. Direction is 0 for
// client to server and 1 for server to client; audio is 0 and video is 1.
// The packet number increases so separate packets use separate starting values.
//
// For the encryption inside the packet, the starting value instead contains
// the 4-byte sender ID, the sender's 8-byte packet number, and four zero bytes.
// A channel key also has its own starting value, called its IV. When present,
// it is combined with these 16 bytes using XOR, which changes each bit where
// the corresponding IV bit is 1. Clients build the same values to decrypt.
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
 * @brief Writes a 64-bit number into eight bytes, with the most significant byte first.
 *
 * @param ubyte* out -> buffer with room for eight bytes
 * @param uint64 value -> number to write
 *
 * @return void
 */
static void _crypto_media_internal__write_u64(ubyte *out, uint64 value)
{
    uint64 i = 0;

    for (i = 0; i < 8; i++)
    {
        out[i] = (ubyte)(value >> (56 - i * 8));
    }
}

/**
 * @brief Reads eight bytes as a 64-bit number, with the most significant byte first.
 *
 * @param const ubyte* in -> buffer containing at least eight bytes
 *
 * @return uint64 -> number stored in those eight bytes.
 */
static uint64 _crypto_media_internal__read_u64(const ubyte *in)
{
    uint64 value = 0;
    uint64 i = 0;

    for (i = 0; i < 8; i++)
    {
        value = (value << 8) | in[i];
    }
    return value;
}

/**
 * @brief Builds the 16-byte AES starting value for one audio or video packet on a connection.
 *
 * @param uint64 frame_counter -> connection packet number written into the first eight bytes
 * @param int64 direction -> MEDIA_DIRECTION_FROM_CLIENT or MEDIA_DIRECTION_TO_CLIENT
 * @param int64 stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param ubyte* out_block -> caller-owned buffer with room for the 16-byte starting value
 *
 * @return void
 */
static void _crypto_media_internal__session_block(uint64 frame_counter, int64 direction, int64 stream, ubyte *out_block)
{
    clib__null_memory(out_block, 16);
    _crypto_media_internal__write_u64(out_block, frame_counter);
    out_block[8] = (ubyte)direction;
    out_block[9] = (ubyte)stream;
}

/**
 * @brief Builds the starting value used to encrypt the audio or video contents inside a packet.
 *
 * @param uint64 sender_id -> sender ID; its low 32 bits are written into the starting value
 * @param uint64 sender_counter -> packet number assigned by the original sender
 * @param const ubyte* layer_iv -> optional 16-byte starting value belonging to the key, or NULL_POINTER
 * @param ubyte* out_block -> caller-owned buffer with room for the 16-byte result
 *
 * @return void
 */
// builds the counter block of an inner layer from the sender id and the sender counter, combined with
// the key's iv when the key has one (channel keys do, music bot keys and server keys do not)
void crypto_media__inner_block(uint64 sender_id, uint64 sender_counter, const ubyte *layer_iv, ubyte *out_block)
{
    uint64 i = 0;

    clib__null_memory(out_block, 16);
    out_block[0] = (ubyte)sender_id;
    out_block[1] = (ubyte)(sender_id >> 8);
    out_block[2] = (ubyte)(sender_id >> 16);
    out_block[3] = (ubyte)(sender_id >> 24);
    _crypto_media_internal__write_u64(out_block + 4, sender_counter);

    // a channel key comes with its own 16 byte iv (a starting value chosen when the key was made); it is
    // combined into the block so that the key's iv still takes part. keys without an iv pass NULL_POINTER
    if (layer_iv != NULL_POINTER)
    {
        for (i = 0; i < 16; i++)
        {
            out_block[i] ^= layer_iv[i];
        }
    }
}

/**
 * @brief Applies one key to the audio or video contents using the sender ID and packet number.
 *
 * @param const crypto_key_t* key -> encryption key for this layer
 * @param const ubyte* layer_iv -> optional 16-byte starting value for the key, or NULL_POINTER
 * @param uint64 sender_id -> ID of the client or music bot that produced the packet
 * @param uint64 sender_counter -> packet number assigned by that sender
 * @param ubyte* data -> caller-owned contents changed in place
 * @param uint64 size -> number of bytes in data
 *
 * @return boole -> TRUE when the encryption method accepted the key and processed the data; otherwise FALSE.
 */
// Encrypt the supplied bytes with this key's chosen encryption method. Build
// the starting value from the sender ID and packet number as described above.
// The encrypted result replaces the bytes in data. Return FALSE if it fails.
boole crypto_media__apply_inner_layer(const crypto_key_t *key, const ubyte *layer_iv, uint64 sender_id, uint64 sender_counter, ubyte *data, uint64 size)
{
    ubyte block[16] = { 0 };

    crypto_media__inner_block(sender_id, sender_counter, layer_iv, block);
    return crypto_metadata__crypt(key, block, sizeof(block), data, size, CRYPTO_ENCRYPT);
}

/**
 * @brief Writes the sender ID and original packet number before the encrypted audio or video
 *        contents.
 *
 * @param ubyte* body -> buffer with room for the 12-byte body header
 * @param uint64 sender_id -> sender ID; its low 32 bits are written with the least significant byte first
 * @param uint64 sender_counter -> sender packet number, written with the most significant byte first
 *
 * @return void
 */
// writes the first 12 bytes of a body that the server builds itself (the music bot frames)
void crypto_media__write_body_header(ubyte *body, uint64 sender_id, uint64 sender_counter)
{
    body[0] = (ubyte)sender_id;
    body[1] = (ubyte)(sender_id >> 8);
    body[2] = (ubyte)(sender_id >> 16);
    body[3] = (ubyte)(sender_id >> 24);
    _crypto_media_internal__write_u64(body + 4, sender_counter);
}

/**
 * @brief Checks a received packet and removes its connection encryption while keeping its channel
 *        encryption intact.
 *
 * @param client_t* sender -> client whose keys protect the packet and whose ID must appear in its body
 * @param int64 stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param const ubyte* frame -> received packet containing its header, encrypted body, and check value
 * @param uint64 size -> number of bytes in frame
 * @param ubyte* out_body -> caller-owned buffer that receives the sender header and still-encrypted channel contents
 * @param uint64 out_capacity -> size of out_body in bytes
 *
 * @attention The caller must keep the sender and its keys stable, normally with the clients lock. Ignore out_body when zero is returned; it may contain a partial result.
 *
 * @return uint64 -> number of body bytes written, or zero when the packet is invalid, cannot fit, or cannot be decrypted.
 */
// Check a packet received from a client, remove the encryption used for that
// connection, and check that the sender ID inside belongs to that client.
// out_body receives the sender ID, sender's packet number, and the audio or
// video bytes still encrypted with the channel keys. Return the number of bytes
// written to out_body, or 0 if the packet is rejected.
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

    // Remove the encryption used between this client and the server. Recreate
    // its starting value from the packet number, direction and audio/video
    // choice, then decrypt with the key agreed on when this client connected.
    clib__memcpy(out_body, frame + MEDIA_FRAME_HEADER, body_size);
    _crypto_media_internal__session_block(_crypto_media_internal__read_u64(frame + 1), MEDIA_DIRECTION_FROM_CLIENT, stream, block);
    if (!crypto_metadata__crypt_session(&sender->session_keys, block, sizeof(block), out_body, body_size, CRYPTO_DECRYPT))
    {
        return 0;
    }

    // the sender id inside must be the client this socket or data channel belongs to; a client cannot
    // make the others believe that somebody else is talking
    sender_id_in_body = (uint64)out_body[0] | ((uint64)out_body[1] << 8) | ((uint64)out_body[2] << 16) | ((uint64)out_body[3] << 24);
    if (sender_id_in_body != sender->client_id)
    {
        DBG_MEDIA_AUTH log_info("%s %llu %s %llu", "frame of client", sender->client_id, "refused: it names sender", sender_id_in_body);
        return 0;
    }
    crypto_media__inner_block(sender_id_in_body, _crypto_media_internal__read_u64(out_body + 4), NULL_POINTER, block);
    if (sender->password_keys != NULL_POINTER && !crypto_metadata__crypt_layers(sender->password_keys->keys, sender->password_keys->count, block, sizeof(block), out_body + MEDIA_BODY_HEADER, body_size - MEDIA_BODY_HEADER, CRYPTO_DECRYPT))
    {
        return 0;
    }
    return body_size;
}

/**
 * @brief Protects a copy of an audio or video body with the receiving client's connection keys.
 *
 * @param client_t* receiver -> client whose keys are used and whose outgoing packet counter is increased
 * @param int64 stream -> MEDIA_STREAM_AUDIO or MEDIA_STREAM_VIDEO
 * @param const ubyte* body -> sender header followed by contents already encrypted for the channel
 * @param uint64 body_size -> number of bytes in body
 * @param ubyte* out_frame -> caller-owned buffer that receives the packet header, encrypted body, and check value
 * @param uint64 out_capacity -> size of out_frame in bytes
 *
 * @attention The caller must keep the client and its keys stable, normally with the clients lock. The counter is increased atomically, so several senders can prepare packets for this client.
 *
 * @return uint64 -> total packet length in bytes, or zero when keys are unavailable, output cannot fit, or encryption fails.
 */
// Prepare a packet for one receiving client. Encrypt its contents with that
// client's connection key and add the check value that lets the client detect
// changes. Return the number of bytes written to out_frame, or 0 if encryption
// fails or the packet does not fit in the supplied buffer.
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

    status0 = receiver->session_keys.is_ready == FALSE;
    status1 = body_size < MEDIA_BODY_HEADER;

    // Check by subtraction before adding framing bytes or narrowing a copy length.
    status2 = out_capacity < MEDIA_FRAME_HEADER + MEDIA_FRAME_CODE || body_size > out_capacity - MEDIA_FRAME_HEADER - MEDIA_FRAME_CODE || body_size > SIZE_MAX - MEDIA_FRAME_HEADER - MEDIA_FRAME_CODE || body_size > INT64_MAX;
    if (status0 == TRUE || status1 == TRUE || status2 == TRUE)
    {
        return 0;
    }
    frame_size = MEDIA_FRAME_HEADER + body_size + MEDIA_FRAME_CODE;

    // Several threads can send to this client. Increase the counter in one atomic
    // operation so they cannot assign the same packet number to different frames.
    frame_counter = atomic_fetch_add_explicit(&receiver->media_send_counter, 1, memory_order_relaxed);
    out_frame[0] = MEDIA_FRAME_VERSION;
    _crypto_media_internal__write_u64(out_frame + 1, frame_counter);
    clib__memcpy(out_frame + MEDIA_FRAME_HEADER, body, body_size);
    sender_id = (uint64)body[0] | ((uint64)body[1] << 8) | ((uint64)body[2] << 16) | ((uint64)body[3] << 24);
    crypto_media__inner_block(sender_id, _crypto_media_internal__read_u64(body + 4), NULL_POINTER, block);
    if (receiver->password_keys != NULL_POINTER && !crypto_metadata__crypt_layers(receiver->password_keys->keys, receiver->password_keys->count, block, sizeof(block), out_frame + MEDIA_FRAME_HEADER + MEDIA_BODY_HEADER, body_size - MEDIA_BODY_HEADER, CRYPTO_ENCRYPT))
    {
        return 0;
    }
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
