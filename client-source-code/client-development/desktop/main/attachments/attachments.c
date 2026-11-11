#include "attachments/attachments_internal.h"
#include "attachments/attachment_file.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>

// declarations
static void _attachments_internal__worker(void *context);
static uint64 _attachments_internal__cache_size(const desktop_attachment_record_t *record);
static boole _attachments_internal__make_room(desktop_attachments_t *attachments, uint64 id, uint64 required);

/**
 * @brief clears private payloads and keys while retaining the display result.
 * @param desktop_attachment_record_t* record -> exclusively owned record
 * @return void
 */
void desktop_attachment_record__release(desktop_attachment_record_t *record)
{
    // erase bytes and pixels
    if (record->bytes)
    {
        mbedtls_platform_zeroize(record->bytes, record->bytes_capacity);
    }
    if (record->pixels)
    {
        mbedtls_platform_zeroize(record->pixels, (uint64)record->view.image_width * (uint64)record->view.image_height * 4);
    }
    free(record->bytes);
    free(record->pixels);

    // free the encrypted texts
    if (record->wire && record->wire_capacity)
    {
        mbedtls_platform_zeroize(record->wire, record->wire_capacity);
    }
    crypto_session__free_text(record->wire);
    crypto_session__free_text(record->header);
    crypto_session__free_text(record->preview);

    // reset pointers and sizes
    record->bytes = NULL;
    record->pixels = NULL;
    record->wire = NULL;
    record->header = NULL;
    record->preview = NULL;
    record->wire_size = 0;
    record->wire_capacity = 0;
    record->bytes_capacity = 0;

    // erase keys and path
    mbedtls_platform_zeroize(&record->keys, sizeof(record->keys));
    mbedtls_platform_zeroize(&record->previous, sizeof(record->previous));
    mbedtls_platform_zeroize(record->path, sizeof(record->path));
    mbedtls_platform_zeroize(record->public_key, sizeof(record->public_key));
}

/**
 * @brief releases every owned buffer before reusing a bounded record slot.
 * @param desktop_attachment_record_t* record -> exclusively owned record
 * @return void
 */
void desktop_attachment_record__clear(desktop_attachment_record_t *record)
{
    desktop_attachment_record__release(record);
    mbedtls_platform_zeroize(record, sizeof(*record));
}

/**
 * @brief reports failure without retaining unusable ciphertext or private keys.
 * @param desktop_attachment_record_t* record -> exclusively owned record
 * @param const char* detail -> bounded display reason
 * @return void
 */
void desktop_attachment_record__fail(desktop_attachment_record_t *record, const char *detail)
{
    // free buffers and keys
    desktop_attachment_record__release(record);

    // store the failure state
    record->view.state = DESKTOP_ATTACHMENT_FAILED;
    record->job = ATTACHMENT_JOB_NONE;
    clib__string_copy(record->view.detail, sizeof(record->view.detail), detail);
}

/**
 * @brief finds a stable attachment ID; caller holds the manager mutex.
 * @param desktop_attachments_t* attachments -> manager
 * @param uint64 id -> nonzero attachment ID
 * @return desktop_attachment_record_t* -> borrowed slot or NULL
 */
desktop_attachment_record_t *desktop_attachments__find(desktop_attachments_t *attachments, uint64 id)
{
    int64 index = 0;

    // search by ID
    for (index = 0; id && index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        if (attachments->records[index].view.id == id)
        {
            return &attachments->records[index];
        }
    }
    return NULL;
}

/**
 * @brief reserves an empty or oldest completed slot without evicting a worker-owned transfer.
 * @param desktop_attachments_t* attachments -> manager; caller holds its mutex
 * @return desktop_attachment_record_t* -> initialized slot or NULL when all are active
 */
desktop_attachment_record_t *desktop_attachments__reserve(desktop_attachments_t *attachments)
{
    desktop_attachment_record_t *selected = NULL_POINTER;
    desktop_attachment_record_t *record = NULL_POINTER;
    int64 index = 0;

    // choose empty or oldest
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        record = &attachments->records[index];
        if (!record->view.id)
        {
            selected = record;
            break;
        }
        if (!record->busy && !record->job && record->view.id != attachments->uploading && record->view.state >= DESKTOP_ATTACHMENT_READY && (!selected || record->view.id < selected->view.id))
        {
            selected = record;
        }
    }

    // reset the chosen record
    if (selected)
    {
        desktop_attachment_record__clear(selected);
        selected->view.id = ++attachments->next_id;
        selected->view.epoch = attachments->epoch;
        selected->view.peer = -1;
        selected->view.channel = -1;
        selected->view.server_id = -1;
        selected->sender = -1;
        selected->deadline = desktop_system__milliseconds() + 600000;
        attachments->revision++;
    }
    return selected;
}

/**
 * @brief counts retained original bytes and preview pixels, including work borrowed from this record.
 * @param const desktop_attachment_record_t* record -> manager or worker-owned record
 * @return uint64 -> bounded retained-byte estimate
 */
static uint64 _attachments_internal__cache_size(const desktop_attachment_record_t *record)
{
    return (record->bytes || record->busy ? record->view.size : 0) + (record->pixels || record->busy ? (uint64)record->view.image_width * (uint64)record->view.image_height * 4 : 0);
}

/**
 * @brief evicts older completed payloads until a newly completed attachment fits the cache budget.
 * @param desktop_attachments_t* attachments -> locked manager
 * @param uint64 id -> record being committed
 * @param uint64 required -> new retained byte count
 * @return boole -> TRUE when the bounded cache can retain the payload
 */
static boole _attachments_internal__make_room(desktop_attachments_t *attachments, uint64 id, uint64 required)
{
    uint64 total = 0;
    int64 index = 0;
    desktop_attachment_record_t *oldest = NULL_POINTER;
    desktop_attachment_record_t *record = NULL_POINTER;

    // repeat until it fits
    while (TRUE)
    {
        // count size, find oldest
        total = required;
        oldest = NULL;
        for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
        {
            record = &attachments->records[index];
            if (record->view.id == id)
            {
                continue;
            }
            total += _attachments_internal__cache_size(record);
            if (!record->busy && !record->job && record->view.state == DESKTOP_ATTACHMENT_READY && record->bytes && (!oldest || record->view.id < oldest->view.id))
            {
                oldest = record;
            }
        }

        // the size fits
        if (total <= DESKTOP_ATTACHMENT_CACHE_LIMIT)
        {
            return TRUE;
        }

        // nothing can be released
        if (!oldest)
        {
            return FALSE;
        }

        // release the oldest record
        desktop_attachment_record__fail(oldest, "Attachment released from memory. Ask the sender to resend it if needed.");
    }
}

/**
 * @brief owns file I/O, RSA/content processing and image decoding; GUI and protocol threads exchange bounded records.
 * @param void* context -> manager retained until this worker joins
 * @return void
 */
static void _attachments_internal__worker(void *context)
{
    desktop_attachments_t *attachments = context;
    desktop_crypto_t *crypto = NULL_POINTER;
    desktop_crypto_t *replacement = NULL_POINTER;
    desktop_attachment_record_t work = { 0 };
    desktop_attachment_record_t *record = NULL_POINTER;
    int64 index = 0;
    int64 job = 0;
    boole stop = FALSE;
    boole saved = FALSE;

    // loop until stopped
    while (TRUE)
    {
        // clear the work record
        clib__null_memory(&work, sizeof(work));
        job = ATTACHMENT_JOB_NONE;

        // lock and read flags
        desktop_mutex__lock(attachments->mutex);
        stop = attachments->stop;
        replacement = attachments->next_crypto;
        attachments->next_crypto = NULL;

        // find a waiting job
        for (index = 0; !stop && index < DESKTOP_ATTACHMENT_COUNT; index++)
        {
            record = &attachments->records[index];
            if (record->job && !record->busy && (record->job == ATTACHMENT_JOB_SAVE || attachments->connected))
            {
                // copy the record
                work = *record;
                job = record->job;
                record->job = ATTACHMENT_JOB_NONE;
                record->busy = TRUE;

                // clear the record pointers
                record->bytes = NULL;
                record->pixels = NULL;
                record->wire = NULL;
                record->header = NULL;
                record->preview = NULL;

                // erase the record keys
                mbedtls_platform_zeroize(&record->keys, sizeof(record->keys));
                mbedtls_platform_zeroize(&record->previous, sizeof(record->previous));
                mbedtls_platform_zeroize(record->path, sizeof(record->path));
                mbedtls_platform_zeroize(record->public_key, sizeof(record->public_key));
                break;
            }
        }
        desktop_mutex__unlock(attachments->mutex);

        // use the new crypto
        if (replacement)
        {
            crypto_session__destroy(crypto);
            crypto = replacement;
            replacement = NULL;
        }

        // stop was requested
        if (stop)
        {
            break;
        }

        // wait for a job
        if (!job)
        {
            desktop_system__sleep(10);
            continue;
        }

        // run the job
        if (job == ATTACHMENT_JOB_SAVE)
        {
            // write the file
            saved = desktop_attachment_file__save(work.path, work.bytes, work.view.size);
            mbedtls_platform_zeroize(work.path, sizeof(work.path));
            work.view.saved |= saved;
            clib__string_copy(work.view.detail, sizeof(work.view.detail), saved ? "Saved" : "Could not save. Choose a writable folder and a new filename.");
        }
        else if (!crypto)
        {
            desktop_attachment_record__fail(&work, "The secure attachment worker is unavailable.");
        }
        else if (job == ATTACHMENT_JOB_PACK)
        {
            // encrypt the selected file
            desktop_attachment_codec__pack(&work, crypto);
        }
        else if (job == ATTACHMENT_JOB_DECODE)
        {
            // decrypt the received data
            desktop_attachment_codec__decode(&work, crypto);
        }
        else if (job == ATTACHMENT_JOB_OFFER)
        {
            // check the received offer
            desktop_attachment_codec__offer(&work, crypto);
        }

        // store the result
        desktop_mutex__lock(attachments->mutex);
        record = desktop_attachments__find(attachments, work.view.id);
        if (record && record->view.epoch == attachments->epoch && record->view.state != DESKTOP_ATTACHMENT_CANCELLED)
        {
            // check the cache limit
            if (!_attachments_internal__make_room(attachments, work.view.id, _attachments_internal__cache_size(&work)))
            {
                desktop_attachment_record__fail(&work, "Attachment memory limit reached. Finish another transfer first.");
            }

            // copy the work back
            work.job = ATTACHMENT_JOB_NONE;
            work.busy = FALSE;
            work.decision = record->decision ? record->decision : work.decision;
            *record = work;
            mbedtls_platform_zeroize(&work, sizeof(work));
        }
        else if (record)
        {
            record->busy = FALSE;
        }
        attachments->revision++;
        desktop_mutex__unlock(attachments->mutex);

        // free the work record
        desktop_attachment_record__clear(&work);
    }

    // destroy the crypto
    crypto_session__destroy(crypto);
}

/**
 * @brief creates the stable bounded attachment store and its sleeping worker.
 * @return desktop_attachments_t* -> owned manager or NULL
 */
desktop_attachments_t *desktop_attachments__create(void)
{
    desktop_attachments_t *attachments = calloc(1, sizeof(*attachments));

    // check the allocation
    if (!attachments)
    {
        return NULL;
    }

    // create the mutex
    attachments->mutex = desktop_mutex__create();
    attachments->revision = 1;
    attachments->preferences_revision = 1;

    // start the worker thread
    if (attachments->mutex)
    {
        attachments->thread = desktop_thread__start(_attachments_internal__worker, attachments);
    }

    // destroy on failure
    if (!attachments->thread)
    {
        desktop_attachments__destroy(attachments);
        return NULL;
    }
    return attachments;
}

/**
 * @brief stops worker ownership before erasing cached files and cryptographic state.
 * @param desktop_attachments_t* attachments -> owned manager, or NULL
 * @return void
 */
void desktop_attachments__destroy(desktop_attachments_t *attachments)
{
    int64 index = 0;

    // nothing to destroy
    if (!attachments)
    {
        return;
    }

    // set the stop flag
    if (attachments->mutex)
    {
        desktop_mutex__lock(attachments->mutex);
        attachments->stop = TRUE;
        desktop_mutex__unlock(attachments->mutex);
    }

    // wait for the thread
    desktop_thread__join(attachments->thread);

    // clear every record
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        desktop_attachment_record__clear(&attachments->records[index]);
    }

    // free the manager
    crypto_session__destroy(attachments->next_crypto);
    desktop_mutex__destroy(attachments->mutex);
    mbedtls_platform_zeroize(attachments, sizeof(*attachments));
    free(attachments);
}

/**
 * @brief starts a new connection generation without sharing mutable RSA state across threads.
 * @param desktop_attachments_t* attachments -> live manager
 * @param const desktop_crypto_t* crypto -> authenticated session identity
 * @return boole -> TRUE when the new worker identity was copied
 */
boole desktop_attachments__start(desktop_attachments_t *attachments, const desktop_crypto_t *crypto)
{
    desktop_crypto_t *copy = crypto_session__copy_identity(crypto);
    int64 index = 0;

    // check the copy
    if (!copy)
    {
        return FALSE;
    }

    // start a new epoch
    desktop_mutex__lock(attachments->mutex);
    attachments->epoch++;
    attachments->connected = TRUE;
    attachments->uploading = 0;
    attachments->preferences_sent = 0;

    // clear or cancel records
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        if (!attachments->records[index].busy)
        {
            desktop_attachment_record__clear(&attachments->records[index]);
        }
        else
        {
            attachments->records[index].view.state = DESKTOP_ATTACHMENT_CANCELLED;
        }
    }

    // store the new identity
    crypto_session__destroy(attachments->next_crypto);
    attachments->next_crypto = copy;
    attachments->revision++;
    desktop_mutex__unlock(attachments->mutex);
    return TRUE;
}

/**
 * @brief cancels network work while retaining completed files for an explicit Save as.
 * @param desktop_attachments_t* attachments -> live manager
 * @return void
 */
void desktop_attachments__disconnect(desktop_attachments_t *attachments)
{
    int64 index = 0;
    desktop_attachment_record_t *record = NULL_POINTER;

    // set disconnected state
    desktop_mutex__lock(attachments->mutex);
    attachments->connected = FALSE;
    attachments->uploading = 0;

    // cancel unfinished transfers
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        record = &attachments->records[index];
        if (record->view.state > DESKTOP_ATTACHMENT_EMPTY && record->view.state < DESKTOP_ATTACHMENT_READY)
        {
            record->view.state = DESKTOP_ATTACHMENT_CANCELLED;
            record->job = ATTACHMENT_JOB_NONE;
            clib__string_copy(record->view.detail, sizeof(record->view.detail), "Disconnected");
            if (!record->busy)
            {
                desktop_attachment_record__release(record);
            }
        }
    }
    attachments->revision++;
    desktop_mutex__unlock(attachments->mutex);
}

/**
 * @brief copies display values only; no payload or key pointer leaves the manager.
 * @param desktop_attachments_t* attachments -> stable manager
 * @param desktop_attachments_view_t* view -> caller-owned snapshot
 * @return boole -> TRUE when the revision changed
 */
boole desktop_attachments__view(desktop_attachments_t *attachments, desktop_attachments_view_t *view)
{
    int64 index = 0;
    boole changed = FALSE;

    // copy a changed view
    desktop_mutex__lock(attachments->mutex);
    if (view->revision != attachments->revision)
    {
        clib__null_memory(view, sizeof(*view));
        view->revision = attachments->revision;
        for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
        {
            view->items[index] = attachments->records[index].view;
        }
        changed = TRUE;
    }
    desktop_mutex__unlock(attachments->mutex);
    return changed;
}

/**
 * @brief queues an explicit decision for a validated incoming offer.
 * @param desktop_attachments_t* attachments -> manager
 * @param uint64 id -> stable UI selection
 * @param boole accept -> TRUE for accept, FALSE for reject
 * @return boole -> TRUE when queued
 */
boole desktop_attachments__decide(desktop_attachments_t *attachments, uint64 id, boole accept)
{
    desktop_attachment_record_t *record = NULL_POINTER;
    boole queued = FALSE;

    // store the decision
    desktop_mutex__lock(attachments->mutex);
    record = desktop_attachments__find(attachments, id);
    if (attachments->connected && record && record->view.state == DESKTOP_ATTACHMENT_OFFERED && !record->decision)
    {
        record->decision = accept ? 1 : 2;
        queued = TRUE;
    }
    desktop_mutex__unlock(attachments->mutex);
    return queued;
}

/**
 * @brief cancels pending or active work without releasing buffers still owned by the worker.
 * @param desktop_attachments_t* attachments -> manager
 * @param uint64 id -> stable UI selection
 * @return boole -> TRUE when cancellation was queued
 */
boole desktop_attachments__cancel(desktop_attachments_t *attachments, uint64 id)
{
    desktop_attachment_record_t *record = NULL_POINTER;
    boole queued = FALSE;

    // store the cancel decision
    desktop_mutex__lock(attachments->mutex);
    record = desktop_attachments__find(attachments, id);
    if (record && record->view.state > DESKTOP_ATTACHMENT_EMPTY && record->view.state < DESKTOP_ATTACHMENT_READY)
    {
        record->decision = 3;
        if (record->busy)
        {
            record->view.state = DESKTOP_ATTACHMENT_CANCELLED;
        }
        queued = TRUE;
        attachments->revision++;
    }
    desktop_mutex__unlock(attachments->mutex);
    return queued;
}

/**
 * @brief copies a destination for asynchronous Save as; incoming names never select filesystem paths.
 * @param desktop_attachments_t* attachments -> manager
 * @param uint64 id -> completed attachment selected by the user
 * @param const char* path -> user-chosen new destination
 * @return boole -> TRUE when queued
 */
boole desktop_attachments__save(desktop_attachments_t *attachments, uint64 id, const char *path)
{
    desktop_attachment_record_t *record = NULL_POINTER;
    boole queued = FALSE;

    // queue the save job
    desktop_mutex__lock(attachments->mutex);
    record = desktop_attachments__find(attachments, id);
    if (record && record->bytes && !record->busy && !record->job && record->view.state == DESKTOP_ATTACHMENT_READY && path && path[0] && clib__string_copy(record->path, sizeof(record->path), path))
    {
        record->job = ATTACHMENT_JOB_SAVE;
        clib__string_copy(record->view.detail, sizeof(record->view.detail), "Saving...");
        attachments->revision++;
        queued = TRUE;
    }
    desktop_mutex__unlock(attachments->mutex);
    return queued;
}

/**
 * @brief supplies a bounded copy of decoded preview pixels for a GUI-owned texture.
 * @param desktop_attachments_t* attachments -> manager
 * @param uint64 id -> attachment ID
 * @param int64* width -> pixel width
 * @param int64* height -> pixel height
 * @return ubyte* -> owned RGBA pixels, or NULL; release with free
 */
ubyte *desktop_attachments__image(desktop_attachments_t *attachments, uint64 id, int64 *width, int64 *height)
{
    desktop_attachment_record_t *record = NULL_POINTER;
    ubyte *pixels = NULL_POINTER;
    uint64 size = 0;

    // clear the outputs
    *width = 0;
    *height = 0;

    // copy the preview pixels
    desktop_mutex__lock(attachments->mutex);
    record = desktop_attachments__find(attachments, id);
    if (record && record->pixels)
    {
        size = (uint64)record->view.image_width * (uint64)record->view.image_height * 4;
        pixels = calloc(1, size);
        if (pixels)
        {
            clib__memcpy(pixels, record->pixels, size);
            *width = record->view.image_width;
            *height = record->view.image_height;
        }
    }
    desktop_mutex__unlock(attachments->mutex);
    return pixels;
}

/**
 * @brief stores local receiving choices; the session forwards their next revision to the server.
 * @param desktop_attachments_t* attachments -> manager
 * @param const desktop_chat_preferences_t* preferences -> copied settings
 * @return void
 */
void desktop_attachments__preferences(desktop_attachments_t *attachments, const desktop_chat_preferences_t *preferences)
{
    int64 index = 0;
    desktop_attachment_record_t *record = NULL_POINTER;
    boole blocked = FALSE;

    // store the preferences
    desktop_mutex__lock(attachments->mutex);
    attachments->preferences = *preferences;
    attachments->preferences_revision++;

    // cancel blocked transfers
    for (index = 0; index < DESKTOP_ATTACHMENT_COUNT; index++)
    {
        // read the block setting
        record = &attachments->records[index];
        blocked = record->view.peer >= 0 ? (record->view.picture ? preferences->block_direct_pictures : preferences->block_direct_files) : (record->view.picture ? preferences->block_channel_pictures : preferences->block_channel_files);

        // cancel the incoming transfer
        if (blocked && !record->view.outgoing && record->view.state > DESKTOP_ATTACHMENT_EMPTY && record->view.state < DESKTOP_ATTACHMENT_READY)
        {
            record->decision = 3;
            record->job = ATTACHMENT_JOB_NONE;
            record->view.state = DESKTOP_ATTACHMENT_CANCELLED;
            clib__string_copy(record->view.detail, sizeof(record->view.detail), "Receiving this attachment type is disabled in local settings.");
            if (!record->busy)
            {
                desktop_attachment_record__release(record);
            }
            attachments->revision++;
        }
    }
    desktop_mutex__unlock(attachments->mutex);
}
