#include "attachments/attachments_internal.h"
#include "attachments/attachment_file.h"
#include "cJSON.h"
#include "network/protocol.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>

// declarations
static char *_attachment_codec_internal__seal(desktop_attachment_record_t *record, desktop_crypto_t *crypto, const char *text, uint64 padding);
static char *_attachment_codec_internal__open(desktop_attachment_record_t *record, desktop_crypto_t *crypto, const char *text, const desktop_content_keys_t *keys);
static char *_attachment_codec_internal__base64(const ubyte *bytes, uint64 size);
static boole _attachment_codec_internal__body(desktop_attachment_record_t *record, const char *plaintext, const char *header);
static cJSON *_attachment_codec_internal__parse(const char *text);

/**
 * @brief limits JSON structure before cJSON can allocate nodes; attachment envelopes contain only small flat objects.
 * @param const char* text -> bounded, terminated decrypted text
 * @return cJSON* -> owned tree or NULL
 */
static cJSON *_attachment_codec_internal__parse(const char *text)
{
    uint64 index = 0;
    int64 depth = 0;
    int64 tokens = 0;
    boole quoted = FALSE;
    boole escaped = FALSE;
    if (!text)
    {
        return NULL;
    }
    for (index = 0; text[index]; index++)
    {
        if (index >= DESKTOP_ATTACHMENT_WIRE_LIMIT)
        {
            return NULL;
        }
        if (quoted)
        {
            if (escaped)
            {
                if (text[index] == 'u' && clib__strncmp(text + index, "u0000", 5) == 0)
                {
                    return NULL;
                }
                escaped = FALSE;
            }
            else if (text[index] == '\\')
            {
                escaped = TRUE;
            }
            else if (text[index] == '"')
            {
                quoted = FALSE;
            }
        }
        else if (text[index] == '"')
        {
            quoted = TRUE;
            tokens++;
        }
        else if (text[index] == '{' || text[index] == '[')
        {
            depth++;
            tokens++;
        }
        else if (text[index] == '}' || text[index] == ']')
        {
            depth--;
        }
        else if (text[index] == ',' || text[index] == ':')
        {
            tokens++;
        }
        if (depth < 0 || depth > 4 || tokens > 64)
        {
            return NULL;
        }
    }
    return !quoted && !depth ? desktop_protocol__parse_json(text, index, DESKTOP_ATTACHMENT_WIRE_LIMIT, 4) : NULL;
}

/**
 * @brief encodes bounded attachment bytes without depending on a locale or text encoding.
 * @param const ubyte* bytes -> borrowed source
 * @param uint64 size -> source length
 * @return char* -> owned base64 string, or NULL
 */
static char *_attachment_codec_internal__base64(const ubyte *bytes, uint64 size)
{
    uint64 capacity = (size + 2) / 3 * 4 + 1;
    size_t written = 0;
    char *result = calloc(1, capacity);
    if (result && mbedtls_base64_encode((ubyte *)result, capacity, &written, bytes, size) != 0)
    {
        mbedtls_platform_zeroize(result, capacity);
        free(result);
        result = NULL;
    }
    return result;
}

/**
 * @brief encrypts channel attachments with a hidden message IV and direct attachments with fresh key IVs.
 * @param desktop_attachment_record_t* record -> worker-owned transfer
 * @param desktop_crypto_t* crypto -> attachment worker's identity and RNG
 * @param const char* text -> plaintext
 * @param uint64 padding -> protocol padding floor
 * @return char* -> owned wire string or NULL
 */
static char *_attachment_codec_internal__seal(desktop_attachment_record_t *record, desktop_crypto_t *crypto, const char *text, uint64 padding)
{
    desktop_content_keys_t keys = { 0 };
    char *encrypted_body = NULL;
    char *result = NULL;

    if (record->view.peer < 0)
    {
        encrypted_body = record->view.picture ? crypto_session__picture_encrypt(crypto, &record->keys, text, padding) : crypto_session__attachment_encrypt(crypto, &record->keys, text, padding);
        result = encrypted_body ? crypto_session__channel_seal(&record->keys, encrypted_body) : NULL;
        crypto_session__free_text(encrypted_body);
        return result;
    }
    if (crypto_session__content_keys(crypto, &keys))
    {
        encrypted_body = crypto_session__direct_content_encrypt(&keys, text, padding, !record->view.picture);
    }
    if (encrypted_body)
    {
        result = crypto_session__direct_seal(crypto, record->public_key, &keys, encrypted_body);
    }
    crypto_session__free_text(encrypted_body);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    return result;
}

/**
 * @brief opens the browser attachment envelope using only the intended recipient's copied identity.
 * @param desktop_attachment_record_t* record -> worker-owned transfer
 * @param desktop_crypto_t* crypto -> worker-owned identity
 * @param const char* text -> bounded encrypted body
 * @param const desktop_content_keys_t* keys -> captured channel-key candidate
 * @return char* -> owned plaintext or NULL
 */
static char *_attachment_codec_internal__open(desktop_attachment_record_t *record, desktop_crypto_t *crypto, const char *text, const desktop_content_keys_t *keys)
{
    cJSON *envelope = NULL;
    desktop_content_keys_t direct = { 0 };
    char *encrypted_body = NULL;
    char *result = NULL;

    if (!text || clib__strlen(text) > DESKTOP_ATTACHMENT_WIRE_LIMIT)
    {
        return NULL;
    }
    if (record->view.peer < 0)
    {
        encrypted_body = crypto_session__channel_open(keys, text);
        if (encrypted_body)
        {
            result = record->view.picture ? crypto_session__attachment_decrypt(keys, encrypted_body) : crypto_session__file_decrypt(keys, encrypted_body);
        }
    }
    else
    {
        envelope = _attachment_codec_internal__parse(text);
        encrypted_body = crypto_session__direct_open(crypto, envelope, &direct);
        if (encrypted_body)
        {
            result = crypto_session__direct_content_decrypt(&direct, encrypted_body, !record->view.picture);
        }
    }
    crypto_session__free_text(encrypted_body);
    crypto_session__free_json(envelope);
    mbedtls_platform_zeroize(&direct, sizeof(direct));
    return result;
}

/**
 * @brief reads, validates and encrypts one selected file on the attachment worker.
 * @param desktop_attachment_record_t* record -> owned work item with immutable destination
 * @param desktop_crypto_t* crypto -> independent worker crypto
 * @return void
 */
void desktop_attachment_codec__pack(desktop_attachment_record_t *record, desktop_crypto_t *crypto)
{
    const char *mime = NULL;
    char *base64 = NULL;
    char *plaintext = NULL;
    char *header = NULL;
    char *offer = NULL;
    cJSON *root = NULL;
    ubyte random[16] = { 0 };
    static const char hex[] = "0123456789abcdef";
    uint64 index = 0;
    uint64 capacity = 0;
    boole complete = FALSE;
    record->bytes = desktop_attachment_file__read(record->path, record->raw_limit, &record->view.size);
    record->bytes_capacity = record->view.size;
    if (!record->bytes)
    {
        desktop_attachment_record__fail(record, "Could not read this file, or it exceeds the allowed size.");
        return;
    }
    desktop_attachment_file__name(record->view.name, sizeof(record->view.name), record->path);
    mime = record->view.picture ? desktop_attachment_image__mime(record->bytes, record->view.size) : "application/octet-stream";
    if (record->view.picture)
    {
        record->pixels = desktop_attachment_image__decode(record->bytes, record->view.size, &record->view.image_width, &record->view.image_height);
        if (!mime || !record->pixels)
        {
            desktop_attachment_record__fail(record, "Cannot preview this image. Use File for unsupported or oversized images.");
            return;
        }
    }
    clib__string_copy(record->view.mime, sizeof(record->view.mime), mime);
    base64 = _attachment_codec_internal__base64(record->bytes, record->view.size);
    if (!base64)
    {
        goto label_finished;
    }
    if (record->view.picture)
    {
        capacity = clib__strlen(base64) + 64;
        plaintext = calloc(1, capacity);
        if (plaintext)
        {
            clib__string_format(plaintext, capacity, "data:%s;base64,%s", mime, base64);
        }
    }
    else
    {
        root = cJSON_CreateObject();
        if (root && cJSON_AddStringToObject(root, "name", record->view.name) && cJSON_AddNumberToObject(root, "size", (double)record->view.size) && cJSON_AddStringToObject(root, "mime", mime))
        {
            header = cJSON_PrintUnformatted(root);
            if (cJSON_AddStringToObject(root, "data", base64))
            {
                plaintext = cJSON_PrintUnformatted(root);
            }
        }
        crypto_session__free_json(root);
        root = NULL;
        record->header = header ? _attachment_codec_internal__seal(record, crypto, header, 1024) : NULL;
    }
    record->wire = plaintext ? _attachment_codec_internal__seal(record, crypto, plaintext, 4096) : NULL;
    complete = record->wire && (record->view.picture || record->header);
    if (complete && record->view.peer >= 0)
    {
        complete = crypto_session__random(crypto, random, sizeof(random));
        for (index = 0; index < sizeof(random); index++)
        {
            record->view.offer[index * 2] = hex[random[index] >> 4];
            record->view.offer[index * 2 + 1] = hex[random[index] & 15];
        }
        root = cJSON_CreateObject();
        if (complete && root && cJSON_AddStringToObject(root, "offer_id", record->view.offer) && cJSON_AddBoolToObject(root, "is_picture", record->view.picture) && cJSON_AddStringToObject(root, "name", record->view.name) && cJSON_AddNumberToObject(root, "size", (double)record->view.size))
        {
            offer = cJSON_PrintUnformatted(root);
        }
        record->preview = offer ? _attachment_codec_internal__seal(record, crypto, offer, 1024) : NULL;
        complete = complete && record->preview;
    }
    record->wire_size = complete ? clib__strlen(record->wire) : 0;
    record->wire_capacity = record->wire_size ? record->wire_size + 1 : 0;
    complete = complete && record->wire_size <= DESKTOP_ATTACHMENT_WIRE_LIMIT;
label_finished:
    crypto_session__free_json(root);
    crypto_session__free_text(base64);
    crypto_session__free_text(plaintext);
    crypto_session__free_text(header);
    crypto_session__free_text(offer);
    mbedtls_platform_zeroize(random, sizeof(random));
    mbedtls_platform_zeroize(record->path, sizeof(record->path));
    mbedtls_platform_zeroize(record->public_key, sizeof(record->public_key));
    mbedtls_platform_zeroize(&record->keys, sizeof(record->keys));
    mbedtls_platform_zeroize(&record->previous, sizeof(record->previous));
    if (complete)
    {
        record->view.state = record->view.peer >= 0 ? DESKTOP_ATTACHMENT_WAITING : DESKTOP_ATTACHMENT_UPLOADING;
        clib__string_copy(record->view.detail, sizeof(record->view.detail), record->view.peer >= 0 ? "Waiting for acceptance" : "Uploading");
    }
    else
    {
        desktop_attachment_record__fail(record, "Could not encrypt the attachment.");
    }
}

/**
 * @brief validates metadata and decoded length before retaining incoming bytes or allocating an image preview.
 * @param desktop_attachment_record_t* record -> worker-owned incoming transfer
 * @param const char* plaintext -> decrypted picture data URL or file JSON
 * @param const char* header -> decrypted file header, NULL for pictures
 * @return boole -> TRUE for a complete validated body
 */
static boole _attachment_codec_internal__body(desktop_attachment_record_t *record, const char *plaintext, const char *header)
{
    cJSON *root = NULL;
    cJSON *metadata = NULL;
    const cJSON *name = NULL;
    const cJSON *size = NULL;
    const cJSON *data = NULL;
    const cJSON *header_name = NULL;
    const cJSON *header_size = NULL;
    const char *encoded = NULL;
    const char *separator = NULL;
    const char *mime = NULL;
    uint64 length = 0;
    uint64 capacity = 0;
    uint64 maximum = record->view.picture ? DESKTOP_ATTACHMENT_IMAGE_LIMIT : DESKTOP_ATTACHMENT_RAW_LIMIT;
    size_t written = 0;
    ubyte *bytes = NULL;
    ubyte *pixels = NULL;
    int64 width = 0;
    int64 height = 0;
    boole valid = FALSE;
    if (!plaintext)
    {
        return FALSE;
    }
    if (record->view.picture)
    {
        separator = clib__strchr(plaintext, ',');
        if (!separator || separator - plaintext > 40 || clib__strncmp(plaintext, "data:image/", 11) != 0 || separator - plaintext < 7 || clib__memcmp(separator - 7, ";base64", 7) != 0)
        {
            goto label_finished;
        }
        encoded = separator + 1;
    }
    else
    {
        root = _attachment_codec_internal__parse(plaintext);
        metadata = header ? _attachment_codec_internal__parse(header) : NULL;
        name = cJSON_GetObjectItemCaseSensitive(root, "name");
        size = cJSON_GetObjectItemCaseSensitive(root, "size");
        data = cJSON_GetObjectItemCaseSensitive(root, "data");
        header_name = cJSON_GetObjectItemCaseSensitive(metadata, "name");
        header_size = cJSON_GetObjectItemCaseSensitive(metadata, "size");
        if (!cJSON_IsString(name) || clib__strlen(name->valuestring) > 255 || !cJSON_IsNumber(size) || !(size->valuedouble >= 1 && size->valuedouble <= (double)maximum) || size->valuedouble != (double)(uint64)size->valuedouble || !cJSON_IsString(data) || !cJSON_IsString(header_name) || !cJSON_IsNumber(header_size) || clib__strcmp(name->valuestring, header_name->valuestring) != 0 || size->valuedouble != header_size->valuedouble)
        {
            goto label_finished;
        }
        encoded = data->valuestring;
    }
    length = clib__strlen(encoded);
    capacity = length / 4 * 3 + 3;
    if (!length || length > (maximum + 2) / 3 * 4 || capacity > maximum + 3)
    {
        goto label_finished;
    }
    bytes = calloc(1, capacity);
    if (!bytes || mbedtls_base64_decode(bytes, capacity, &written, (const ubyte *)encoded, length) != 0 || !written || written > maximum || (!record->view.picture && (double)written != size->valuedouble))
    {
        goto label_finished;
    }
    if (record->view.picture)
    {
        mime = desktop_attachment_image__mime(bytes, written);
        pixels = desktop_attachment_image__decode(bytes, written, &width, &height);
        if (!mime || !pixels)
        {
            goto label_finished;
        }
        clib__string_format(record->view.name, sizeof(record->view.name), "picture-%llu.%s", record->view.id, clib__strcmp(mime, "image/jpeg") == 0 ? "jpg" : mime + 6);
    }
    else
    {
        mime = "application/octet-stream";
        desktop_attachment_file__name(record->view.name, sizeof(record->view.name), name->valuestring);
    }
    clib__string_copy(record->view.mime, sizeof(record->view.mime), mime);
    record->view.size = written;
    record->view.image_width = width;
    record->view.image_height = height;
    record->bytes = bytes;
    record->bytes_capacity = capacity;
    record->pixels = pixels;
    bytes = NULL;
    pixels = NULL;
    valid = TRUE;
label_finished:
    if (bytes)
    {
        mbedtls_platform_zeroize(bytes, capacity);
    }
    free(bytes);
    if (pixels)
    {
        mbedtls_platform_zeroize(pixels, (uint64)width * (uint64)height * 4);
        free(pixels);
    }
    crypto_session__free_json(root);
    crypto_session__free_json(metadata);
    return valid;
}

/**
 * @brief decrypts a complete transfer with the keys captured when its metadata arrived.
 * @param desktop_attachment_record_t* record -> owned ciphertext and metadata
 * @param desktop_crypto_t* crypto -> worker-owned recipient identity
 * @return void
 */
void desktop_attachment_codec__decode(desktop_attachment_record_t *record, desktop_crypto_t *crypto)
{
    char *plaintext = NULL;
    char *header = NULL;
    const desktop_content_keys_t *keys = NULL;
    int64 attempt = 0;
    boole valid = FALSE;
    for (attempt = 0; attempt < (record->view.peer < 0 && record->have_previous ? 2 : 1); attempt++)
    {
        keys = attempt ? &record->previous : &record->keys;
        plaintext = _attachment_codec_internal__open(record, crypto, record->wire, keys);
        header = record->header ? _attachment_codec_internal__open(record, crypto, record->header, keys) : NULL;
        valid = _attachment_codec_internal__body(record, plaintext, header);
        crypto_session__free_text(plaintext);
        crypto_session__free_text(header);
        if (valid)
        {
            break;
        }
    }
    crypto_session__free_text(record->wire);
    crypto_session__free_text(record->header);
    record->wire = NULL;
    record->header = NULL;
    record->wire_size = 0;
    record->wire_capacity = 0;
    record->received = 0;
    mbedtls_platform_zeroize(&record->keys, sizeof(record->keys));
    mbedtls_platform_zeroize(&record->previous, sizeof(record->previous));
    if (valid)
    {
        record->view.state = DESKTOP_ATTACHMENT_READY;
        record->view.progress = 100;
        clib__string_copy(record->view.detail, sizeof(record->view.detail), "Received");
    }
    else
    {
        desktop_attachment_record__fail(record, "Could not decrypt or validate this attachment.");
    }
}

/**
 * @brief validates a direct offer before exposing Accept to the user.
 * @param desktop_attachment_record_t* record -> incoming offer with copied peer lifetime
 * @param desktop_crypto_t* crypto -> worker-owned recipient identity
 * @return void
 */
void desktop_attachment_codec__offer(desktop_attachment_record_t *record, desktop_crypto_t *crypto)
{
    char *plaintext = _attachment_codec_internal__open(record, crypto, record->preview, NULL);
    cJSON *root = plaintext ? _attachment_codec_internal__parse(plaintext) : NULL;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "offer_id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON *picture = cJSON_GetObjectItemCaseSensitive(root, "is_picture");
    uint64 maximum = record->view.picture ? DESKTOP_ATTACHMENT_IMAGE_LIMIT : DESKTOP_ATTACHMENT_RAW_LIMIT;
    if (cJSON_IsString(id) && clib__strcmp(id->valuestring, record->view.offer) == 0 && cJSON_IsString(name) && clib__strlen(name->valuestring) <= 255 && cJSON_IsBool(picture) && cJSON_IsTrue(picture) == record->view.picture && cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble <= (double)maximum && size->valuedouble == (double)(uint64)size->valuedouble)
    {
        desktop_attachment_file__name(record->view.name, sizeof(record->view.name), name->valuestring);
        record->view.size = (uint64)size->valuedouble;
        record->view.state = DESKTOP_ATTACHMENT_OFFERED;
        clib__string_copy(record->view.detail, sizeof(record->view.detail), "Accept or reject this attachment");
    }
    else
    {
        desktop_attachment_record__fail(record, "Invalid or oversized attachment offer.");
        record->decision = 2;
    }
    crypto_session__free_json(root);
    crypto_session__free_text(plaintext);
    crypto_session__free_text(record->preview);
    record->preview = NULL;
}
