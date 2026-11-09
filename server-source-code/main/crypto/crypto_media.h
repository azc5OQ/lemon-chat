#ifndef CRYPTO_MEDIA_H
#define CRYPTO_MEDIA_H 1

#include "../definitions.h"

// Sizes of the parts of the version 2 frame described in crypto_media.c.
#define MEDIA_FRAME_VERSION 2
#define MEDIA_FRAME_HEADER 9 // 1 byte version + 8 bytes frame counter
#define MEDIA_BODY_HEADER 12 // 4 bytes sender id + 8 bytes sender counter, inside the encrypted part
#define MEDIA_FRAME_CODE 8 // the authentication code at the end: the first 8 bytes of the HMAC-SHA256
#define MEDIA_FRAME_OVERHEAD (MEDIA_FRAME_HEADER + MEDIA_BODY_HEADER + MEDIA_FRAME_CODE)
#define MEDIA_STREAM_AUDIO 0
#define MEDIA_STREAM_VIDEO 1

uint64 crypto_media__decrypt_from_client(client_t *sender, int64 stream, const ubyte *frame, uint64 size, ubyte *out_body, uint64 out_capacity);

uint64 crypto_media__encrypt_for_client(client_t *receiver, int64 stream, const ubyte *body, uint64 body_size, ubyte *out_frame, uint64 out_capacity);

void crypto_media__write_body_header(ubyte *body, uint64 sender_id, uint64 sender_counter);

void crypto_media__inner_block(uint64 sender_id, uint64 sender_counter, const ubyte *layer_iv, ubyte *out_block);

boole crypto_media__apply_inner_layer(const crypto_key_t *key, const ubyte *layer_iv, uint64 sender_id, uint64 sender_counter, ubyte *data, uint64 size);

#endif
