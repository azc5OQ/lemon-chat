#ifndef LEMON_DESKTOP_MEDIA_H
#define LEMON_DESKTOP_MEDIA_H

#include "clib.h"


#define DESKTOP_MEDIA_CONTENT 3840
#define DESKTOP_MEDIA_OVERHEAD 29
#define DESKTOP_MEDIA_FRAME (DESKTOP_MEDIA_CONTENT + DESKTOP_MEDIA_OVERHEAD)

typedef struct desktop_media_keys_t
{
    unsigned char encryption[32];
    unsigned char authentication[32];
    unsigned char server[8][32];
    uint64 server_count;
} desktop_media_keys_t;

boole desktop_media__hex(const char* text, unsigned char* output, uint64 size);
boole desktop_media__proof(const unsigned char key[32], const unsigned char ticket[16], uint id, unsigned char output[53]);
boole desktop_media__acknowledgement(const unsigned char key[32], const unsigned char ticket[16], const unsigned char* bytes, uint64 size);
uint64 desktop_media__seal(const desktop_media_keys_t* keys, uint sender, uint64 counter,
    const unsigned char* content, uint64 size, unsigned char* output, uint64 capacity);
uint64 desktop_media__open(const desktop_media_keys_t* keys, const unsigned char* frame, uint64 size,
    uint* sender, uint64* sender_counter, uint64* frame_counter, unsigned char* content, uint64 capacity);
void desktop_media__layer(const unsigned char key[32], const unsigned char* iv, uint sender,
    uint64 counter, unsigned char* data, uint64 size);

#endif
