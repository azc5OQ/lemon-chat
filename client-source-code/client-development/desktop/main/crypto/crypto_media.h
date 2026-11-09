#ifndef LEMON_DESKTOP_CRYPTO_MEDIA_H
#define LEMON_DESKTOP_CRYPTO_MEDIA_H

#include "clib.h"
#include "crypto/keys_session.h"

#define DESKTOP_MEDIA_CONTENT 3840
#define DESKTOP_MEDIA_OVERHEAD 29
#define DESKTOP_MEDIA_FRAME (DESKTOP_MEDIA_CONTENT + DESKTOP_MEDIA_OVERHEAD)

typedef struct desktop_media_keys_t
{
    ubyte encryption[32];
    ubyte authentication[32];
    ubyte server[8][32];
    uint64 server_count;
    int server_types[8];
    lemon_session_keys negotiated;
} desktop_media_keys_t;

boole crypto_media__hex(const char *text, ubyte *output, uint64 size);
boole crypto_media__proof(const ubyte key[32], const ubyte ticket[16], uint id, ubyte output[53]);
boole crypto_media__acknowledgement(const ubyte key[32], const ubyte ticket[16], const ubyte *bytes, uint64 size);
uint64 crypto_media__seal(const desktop_media_keys_t *keys, uint sender, uint64 counter, const ubyte *content, uint64 size, ubyte *output, uint64 capacity);
uint64 crypto_media__open(const desktop_media_keys_t *keys, const ubyte *frame, uint64 size, uint *sender, uint64 *sender_counter, uint64 *frame_counter, ubyte *content, uint64 capacity);
void crypto_media__layer(const ubyte key[32], const ubyte *iv, uint sender, uint64 counter, ubyte *data, uint64 size);

#endif
