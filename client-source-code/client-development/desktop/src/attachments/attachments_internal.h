#ifndef LEMON_DESKTOP_ATTACHMENTS_INTERNAL_H
#define LEMON_DESKTOP_ATTACHMENTS_INTERNAL_H

#include "attachments/attachments.h"
#include "crypto/crypto.h"
#include "platform/thread.h"

enum
{
    ATTACHMENT_JOB_NONE,
    ATTACHMENT_JOB_PACK,
    ATTACHMENT_JOB_DECODE,
    ATTACHMENT_JOB_OFFER,
    ATTACHMENT_JOB_SAVE
};

typedef struct desktop_attachment_record_t
{
    desktop_attachment_t view;
    int64 job;
    int64 decision;
    boole busy;
    boole accepted;
    boole offered;
    boole acknowledged;
    boole completion_sent;
    boole have_previous;
    uint64 deadline;
    uint64 wire_size;
    uint64 received;
    uint64 offset;
    uint64 raw_limit;
    uint64 bytes_capacity;
    uint64 wire_capacity;
    int64 sender;
    uint64 sender_serial;
    unsigned char* bytes;
    unsigned char* pixels;
    char* wire;
    char* header;
    char* preview;
    char path[DESKTOP_ATTACHMENT_PATH];
    char public_key[1369];
    desktop_content_keys_t keys;
    desktop_content_keys_t previous;
} desktop_attachment_record_t;

struct desktop_attachments_t
{
    desktop_mutex_t* mutex;
    desktop_thread_t* thread;
    desktop_crypto_t* next_crypto;
    desktop_attachment_record_t records[DESKTOP_ATTACHMENT_COUNT];
    desktop_chat_preferences_t preferences;
    uint64 preferences_revision;
    uint64 preferences_sent;
    uint64 revision;
    uint64 epoch;
    uint64 next_id;
    uint64 uploading;
    boole connected;
    boole stop;
};

typedef struct desktop_session_t desktop_session_t;
typedef struct cJSON cJSON;

void desktop_attachment_record__clear(desktop_attachment_record_t* record);
void desktop_attachment_record__fail(desktop_attachment_record_t* record, const char* detail);
void desktop_attachment_record__release(desktop_attachment_record_t* record);
desktop_attachment_record_t* desktop_attachments__find(desktop_attachments_t* attachments, uint64 id);
desktop_attachment_record_t* desktop_attachments__reserve(desktop_attachments_t* attachments);
boole desktop_attachments__start(desktop_attachments_t* attachments, const desktop_crypto_t* crypto);
void desktop_attachments__disconnect(desktop_attachments_t* attachments);
void desktop_attachment_codec__pack(desktop_attachment_record_t* record, desktop_crypto_t* crypto);
void desktop_attachment_codec__decode(desktop_attachment_record_t* record, desktop_crypto_t* crypto);
void desktop_attachment_codec__offer(desktop_attachment_record_t* record, desktop_crypto_t* crypto);
unsigned char* desktop_attachment_image__decode(const unsigned char* bytes, uint64 size, int64* width, int64* height);
unsigned char* desktop_attachment_image__decode_bounded(const unsigned char* bytes, uint64 size, int64* width, int64* height, int64 maximum, int64 preview_limit);
const char* desktop_attachment_image__mime(const unsigned char* bytes, uint64 size);
void client_msg__attachments__send(desktop_session_t* session, const desktop_attachment_request_t* request);
void client_msg__attachments__tick(desktop_session_t* session);
void server_msg__attachments__receive(desktop_session_t* session, const cJSON* message);

#endif
