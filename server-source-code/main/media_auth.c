// This file checks audio and video packets that travel between a client and
// the server. When a packet arrives, it checks that the protected bytes have
// not been changed and that the sender ID belongs to the connected client.
// When the server forwards a packet, it protects a separate copy with each
// receiving client's keys. The voice or video inside stays encrypted with the
// channel's keys; the server does not need those keys to forward the packet.
// The encryption and message-checking functions are in metadata_crypto.c.
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
#include "definitions.h"
#include "metadata_crypto.h"

#include "../third-party/dave-g-json/cJSON.h"
#include "base.h" // server settings
#include "../third-party/rxi-log/log.h"

#include "clib/clib_memory.h"

#include "media_auth.h"

#include <string.h>

#define MEDIA_DIRECTION_FROM_CLIENT 0
#define MEDIA_DIRECTION_TO_CLIENT 1

/** @brief writes a 64 bit number into 8 bytes, the most significant byte first */
static void _media_auth_internal__write_u64(unsigned char* out, uint64 value)
{
    int i = 0;

    for (i = 0; i < 8; i++)
    {
        out[i] = (unsigned char)(value >> (56 - i * 8));
    }
}

/** @brief reads 8 bytes, the most significant byte first, as a 64 bit number */
static uint64 _media_auth_internal__read_u64(const unsigned char* in)
{
    uint64 value = 0;
    int i = 0;

    for (i = 0; i < 8; i++)
    {
        value = (value << 8) | in[i];
    }
    return value;
}

/** @brief builds the session counter block described at the top of this file */
static void _media_auth_internal__session_block(uint64 frame_counter, int direction, int stream, unsigned char* out_block)
{
    clib__null_memory(out_block, 16);
    _media_auth_internal__write_u64(out_block, frame_counter);
    out_block[8] = (unsigned char)direction;
    out_block[9] = (unsigned char)stream;
}

void media_auth__inner_block(uint64 sender_id, uint64 sender_counter, const unsigned char* layer_iv, unsigned char* out_block)
{
    int i = 0;

    clib__null_memory(out_block, 16);
    out_block[0] = (unsigned char)sender_id;
    out_block[1] = (unsigned char)(sender_id >> 8);
    out_block[2] = (unsigned char)(sender_id >> 16);
    out_block[3] = (unsigned char)(sender_id >> 24);
    _media_auth_internal__write_u64(out_block + 4, sender_counter);
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

boole media_auth__apply_inner_layer(const crypto_key_t* key, const unsigned char* layer_iv, uint64 sender_id, uint64 sender_counter, unsigned char* data, uint64 size)
{
    unsigned char block[16];

    media_auth__inner_block(sender_id, sender_counter, layer_iv, block);
    return metadata_crypto__crypt(key, block, sizeof(block), data, size, CRYPTO_ENCRYPT);
}

boole media_auth__apply_server_key_layers(uint64 sender_id, uint64 sender_counter, unsigned char* data, uint64 size)
{
    unsigned char block[16];

    // A music bot's audio starts on the server, so the server must encrypt it
    // with the keys from its settings before sending it. An ordinary client's
    // packet already has that encryption when it arrives and can be forwarded.
    media_auth__inner_block(sender_id, sender_counter, NULL_POINTER, block);
    return metadata_crypto__crypt_layers(g_server_settings.keys, g_server_settings.keys_count,
        block, sizeof(block), data, size, CRYPTO_ENCRYPT);
}

void media_auth__write_body_header(unsigned char* body, uint64 sender_id, uint64 sender_counter)
{
    body[0] = (unsigned char)sender_id;
    body[1] = (unsigned char)(sender_id >> 8);
    body[2] = (unsigned char)(sender_id >> 16);
    body[3] = (unsigned char)(sender_id >> 24);
    _media_auth_internal__write_u64(body + 4, sender_counter);
}

uint64 media_auth__decrypt_from_client(client_t* sender, int stream, const unsigned char* frame, uint64 size, unsigned char* out_body, uint64 out_capacity)
{
    unsigned char code[32];
    unsigned char block[16];
    uint64 body_size = 0;
    uint64 sender_id_in_body = 0;

    if (sender->session_keys.is_ready == FALSE || size <= MEDIA_FRAME_OVERHEAD || frame[0] != MEDIA_FRAME_VERSION)
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
    if (metadata_crypto__hmac_sha256(&sender->session_keys.authentication, frame, size - MEDIA_FRAME_CODE, NULL_POINTER, 0, code) == FALSE
        || metadata_crypto__equal(code, frame + size - MEDIA_FRAME_CODE, MEDIA_FRAME_CODE) == FALSE)
    {
        DBG_MEDIA_AUTH log_info("%s %llu %s", "frame of client", sender->client_id, "refused: the authentication code does not match, the frame was changed or made up");
        return 0;
    }
    // Remove the encryption used between this client and the server. Recreate
    // its starting value from the packet number, direction and audio/video
    // choice, then decrypt with the key agreed on when this client connected.
    memcpy(out_body, frame + MEDIA_FRAME_HEADER, body_size);
    _media_auth_internal__session_block(_media_auth_internal__read_u64(frame + 1), MEDIA_DIRECTION_FROM_CLIENT, stream, block);
    if (!metadata_crypto__crypt(&sender->session_keys.encryption, block, sizeof(block), out_body, body_size, CRYPTO_DECRYPT))
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
    return body_size;
}

uint64 media_auth__encrypt_for_client(client_t* receiver, int stream, const unsigned char* body, uint64 body_size, unsigned char* out_frame, uint64 out_capacity)
{
    unsigned char code[32];
    unsigned char block[16];
    uint64 frame_counter = 0;
    uint64 frame_size = MEDIA_FRAME_HEADER + body_size + MEDIA_FRAME_CODE;

    if (receiver->session_keys.is_ready == FALSE || body_size < MEDIA_BODY_HEADER || frame_size > out_capacity)
    {
        return 0;
    }
    // several relay threads may send to the same receiver at the same time (every talking client has its
    // own thread). the counter is read and increased in one step that no other thread can interrupt, so
    // no two frames get the same counter
    frame_counter = __atomic_fetch_add(&receiver->media_send_counter, 1, __ATOMIC_RELAXED);
    out_frame[0] = MEDIA_FRAME_VERSION;
    _media_auth_internal__write_u64(out_frame + 1, frame_counter);
    memcpy(out_frame + MEDIA_FRAME_HEADER, body, body_size);
    _media_auth_internal__session_block(frame_counter, MEDIA_DIRECTION_TO_CLIENT, stream, block);
    if (!metadata_crypto__crypt(&receiver->session_keys.encryption, block, sizeof(block), out_frame + MEDIA_FRAME_HEADER, body_size, CRYPTO_ENCRYPT))
    {
        return 0;
    }
    // the authentication code covers the header and the encrypted part, so neither can be changed
    if (metadata_crypto__hmac_sha256(&receiver->session_keys.authentication, out_frame, MEDIA_FRAME_HEADER + body_size, NULL_POINTER, 0, code) == FALSE)
    {
        return 0;
    }
    memcpy(out_frame + MEDIA_FRAME_HEADER + body_size, code, MEDIA_FRAME_CODE);
    return frame_size;
}
