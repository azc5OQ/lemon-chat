#ifndef LEMON_DESKTOP_ATTACHMENTS_H
#define LEMON_DESKTOP_ATTACHMENTS_H

#include "clib.h"

#define DESKTOP_ATTACHMENT_COUNT 16
#define DESKTOP_ATTACHMENT_PATH 1024
#define DESKTOP_ATTACHMENT_RAW_LIMIT (20ULL * 1024 * 1024)
#define DESKTOP_ATTACHMENT_IMAGE_LIMIT (4ULL * 1024 * 1024)
#define DESKTOP_ATTACHMENT_WIRE_LIMIT (40ULL * 1024 * 1024)
#define DESKTOP_ATTACHMENT_CACHE_LIMIT (64ULL * 1024 * 1024)

typedef enum desktop_attachment_state_e
{
    DESKTOP_ATTACHMENT_EMPTY,
    DESKTOP_ATTACHMENT_PREPARING,
    DESKTOP_ATTACHMENT_WAITING,
    DESKTOP_ATTACHMENT_OFFERED,
    DESKTOP_ATTACHMENT_UPLOADING,
    DESKTOP_ATTACHMENT_RECEIVING,
    DESKTOP_ATTACHMENT_READY,
    DESKTOP_ATTACHMENT_FAILED,
    DESKTOP_ATTACHMENT_CANCELLED
} desktop_attachment_state_e;

typedef struct desktop_chat_preferences_t
{
    int64 image_width;
    int64 image_height;
    int64 scale_percent;
    boole block_channel_pictures;
    boole block_channel_files;
    boole block_direct_pictures;
    boole block_direct_files;
} desktop_chat_preferences_t;

typedef struct desktop_attachment_t
{
    uint64 id;
    uint64 epoch;
    uint64 local_id;
    uint64 peer_serial;
    int64 peer;
    int64 channel;
    int64 server_id;
    int64 progress;
    uint64 size;
    int64 image_width;
    int64 image_height;
    desktop_attachment_state_e state;
    boole outgoing;
    boole picture;
    boole saved;
    char offer[33];
    char name[256];
    char mime[100];
    char detail[192];
} desktop_attachment_t;

typedef struct desktop_attachments_view_t
{
    uint64 revision;
    desktop_attachment_t items[DESKTOP_ATTACHMENT_COUNT];
} desktop_attachments_view_t;

typedef struct desktop_attachment_request_t
{
    int64 peer;
    uint64 peer_serial;
    int64 channel;
    boole picture;
    char path[DESKTOP_ATTACHMENT_PATH];
} desktop_attachment_request_t;

typedef struct desktop_attachments_t desktop_attachments_t;

desktop_attachments_t *desktop_attachments__create(void);
void desktop_attachments__destroy(desktop_attachments_t *attachments);
boole desktop_attachments__view(desktop_attachments_t *attachments, desktop_attachments_view_t *view);
boole desktop_attachments__decide(desktop_attachments_t *attachments, uint64 id, boole accept);
boole desktop_attachments__cancel(desktop_attachments_t *attachments, uint64 id);
boole desktop_attachments__save(desktop_attachments_t *attachments, uint64 id, const char *path);
ubyte *desktop_attachments__image(desktop_attachments_t *attachments, uint64 id, int64 *width, int64 *height);
void desktop_attachments__preferences(desktop_attachments_t *attachments, const desktop_chat_preferences_t *preferences);

#endif
