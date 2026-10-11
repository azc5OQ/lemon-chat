#ifndef MEDIA_AUTH_H
#define MEDIA_AUTH_H 1

#include "definitions.h"

// These functions prepare and check the audio and video packets sent between
// clients and the server. media_auth.c explains which bytes go into each packet
// and how it checks the sender. The encryption functions it calls are in
// metadata_crypto.c, where metadata messages use those same functions too.

// Sizes of the parts of the version 2 frame described in media_auth.c.
#define MEDIA_FRAME_VERSION 2
#define MEDIA_FRAME_HEADER 9                      // 1 byte version + 8 bytes frame counter
#define MEDIA_BODY_HEADER 12                      // 4 bytes sender id + 8 bytes sender counter, inside the encrypted part
#define MEDIA_FRAME_CODE 8                        // the authentication code at the end: the first 8 bytes of the HMAC-SHA256
#define MEDIA_FRAME_OVERHEAD (MEDIA_FRAME_HEADER + MEDIA_BODY_HEADER + MEDIA_FRAME_CODE)
#define MEDIA_STREAM_AUDIO 0
#define MEDIA_STREAM_VIDEO 1

// Check a packet received from a client, remove the encryption used for that
// connection, and check that the sender ID inside belongs to that client.
// out_body receives the sender ID, sender's packet number, and the audio or
// video bytes still encrypted with the channel keys. Return the number of bytes
// written to out_body, or 0 if the packet is rejected.
uint64 media_auth__decrypt_from_client(client_t* sender, int stream, const unsigned char* frame, uint64 size, unsigned char* out_body, uint64 out_capacity);

// Prepare a packet for one receiving client. Encrypt its contents with that
// client's connection key and add the check value that lets the client detect
// changes. Return the number of bytes written to out_frame, or 0 if encryption
// fails or the packet does not fit in the supplied buffer.
uint64 media_auth__encrypt_for_client(client_t* receiver, int stream, const unsigned char* body, uint64 body_size, unsigned char* out_frame, uint64 out_capacity);

// writes the first 12 bytes of a body that the server builds itself (the music bot frames)
void media_auth__write_body_header(unsigned char* body, uint64 sender_id, uint64 sender_counter);

// builds the counter block of an inner layer from the sender id and the sender counter, combined with
// the key's iv when the key has one (channel keys do, music bot keys and server keys do not)
void media_auth__inner_block(uint64 sender_id, uint64 sender_counter, const unsigned char* layer_iv, unsigned char* out_block);

// Encrypt the supplied bytes with this key's chosen encryption method. Build
// the starting value from the sender ID and packet number as described above.
// The encrypted result replaces the bytes in data. Return FALSE if it fails.
boole media_auth__apply_inner_layer(const crypto_key_t* key, const unsigned char* layer_iv, uint64 sender_id, uint64 sender_counter, unsigned char* data, uint64 size);

// Encrypt a music bot's audio with each key from the server settings, in order.
// Return FALSE if any key cannot be used; the caller must then discard the packet.
boole media_auth__apply_server_key_layers(uint64 sender_id, uint64 sender_counter, unsigned char* data, uint64 size);

#endif
