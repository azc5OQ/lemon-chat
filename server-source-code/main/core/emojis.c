#include "../clib/clib.h"
// This file stores the server emojis that users can choose as chat reactions.
#include "emojis.h"
#include "../util.h"
#include <mbedtls/base64.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char id[33];
    char name[33];
    char unicode[8];
    char *base64;
} server_emoji_t;

static const struct
{
    cstring name;
    cstring unicode;
} default_emojis[] = { { "like", "\xf0\x9f\x91\x8d" }, { "love", "\xe2\x9d\xa4\xef\xb8\x8f" }, { "laugh", "\xf0\x9f\x98\x82" }, { "surprise", "\xf0\x9f\x98\xae" }, { "sad", "\xf0\x9f\x98\xa2" }, { "celebrate", "\xf0\x9f\x8e\x89" }, { "grin", "\xf0\x9f\x98\x80" }, { "smile", "\xf0\x9f\x98\x84" }, { "affection", "\xf0\x9f\xa5\xb0" }, { "heart_eyes", "\xf0\x9f\x98\x8d" }, { "cool", "\xf0\x9f\x98\x8e" }, { "thinking", "\xf0\x9f\xa4\x94" }, { "mind_blown", "\xf0\x9f\xa4\xaf" }, { "sleep", "\xf0\x9f\x98\xb4" }, { "cry", "\xf0\x9f\x98\xad" }, { "angry", "\xf0\x9f\x98\xa1" }, { "eyes", "\xf0\x9f\x91\x80" }, { "raised_hands", "\xf0\x9f\x99\x8c" }, { "applause", "\xf0\x9f\x91\x8f" }, { "thanks", "\xf0\x9f\x99\x8f" }, { "strong", "\xf0\x9f\x92\xaa" }, { "fire", "\xf0\x9f\x94\xa5" }, { "hundred", "\xf0\x9f\x92\xaf" }, { "yes", "\xe2\x9c\x85" }, { "no", "\xe2\x9d\x8c" }, { "rocket", "\xf0\x9f\x9a\x80" }, { "music", "\xf0\x9f\x8e\xb5" }, { "lemon", "\xf0\x9f\x8d\x8b" }, { "dislike", "\xf0\x9f\x91\x8e" }, { "handshake", "\xf0\x9f\xa4\x9d" } };

static server_emoji_t server_emojis[MAX_SERVER_EMOJIS];
static pthread_mutex_t server_emojis_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64 server_emoji_image_bytes = 0; // protected by server_emojis_mutex

static boole _emojis_internal__name_valid(cstring name);
static uint64 _emojis_internal__read_be32(const ubyte *bytes);
static boole _emojis_internal__png_valid(const ubyte *bytes, uint64 length);
static boole _emojis_internal__jpeg_valid(const ubyte *bytes, uint64 length);
static boole _emojis_internal__gif_blocks_valid(const ubyte *bytes, uint64 length, uint64 *offset);
static boole _emojis_internal__gif_valid(const ubyte *bytes, uint64 length);
static boole _emojis_internal__image_valid(cstring base64);
static boole _emojis_internal__unicode_valid(cstring unicode);
static boole _emojis_internal__new_id(char *id);
static cstring _emojis_internal__store(cstring name, cstring base64, cstring unicode, cstring id, boole enforce_budget);
static cJSON *_emojis_internal__array(boole include_images);

/**
 * @brief Checks the lowercase name used to find an emoji.
 * @param name Name containing one to 32 letters, digits or underscores.
 */
static boole _emojis_internal__name_valid(cstring name)
{
    uint64 i = 0;

    // refuse an empty name
    if (name == NULL_POINTER || name[0] == 0)
    {
        return FALSE;
    }

    // check every character
    for (i = 0; i <= 32 && name[i] != 0; i++)
    {
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
        {
            return FALSE;
        }
    }

    // check the name length
    return i <= 32;
}

/**
 * @brief Checks an emoji identifier before looking it up.
 * @param id Identifier containing exactly 32 lowercase hexadecimal characters.
 */
boole emojis__is_id_valid(cstring id)
{
    uint64 i = 0;

    // refuse a missing id
    if (id == NULL_POINTER)
    {
        return FALSE;
    }

    // check every character
    for (i = 0; i < 32; i++)
    {
        if (!((id[i] >= 'a' && id[i] <= 'f') || (id[i] >= '0' && id[i] <= '9')))
        {
            return FALSE;
        }
    }

    // check the id length
    return id[32] == 0;
}

/**
 * @brief Reads four image header bytes as an unsigned number.
 * @param bytes Four bytes stored with the largest part first.
 */
static uint64 _emojis_internal__read_be32(const ubyte *bytes)
{
    return ((uint64)bytes[0] << 24) | ((uint64)bytes[1] << 16) | ((uint64)bytes[2] << 8) | bytes[3];
}

/**
 * @brief Checks PNG dimensions, chunk lengths and checksums and rejects animation.
 * @param bytes Decoded PNG file.
 * @param length Number of bytes available.
 */
static boole _emojis_internal__png_valid(const ubyte *bytes, uint64 length)
{
    static const ubyte signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint64 offset = 8;
    uint64 i = 0;
    uint64 chunk = 0;
    uint64 width = 0;
    uint64 height = 0;
    uint64 crc = 0;
    uint64 bit = 0;
    uint64 depth = 0;
    uint64 color = 0;
    boole valid_depth = FALSE;
    boole has_data = FALSE;

    // check the PNG signature
    if (length < 45 || clib__memcmp(bytes, signature, 8) != 0)
    {
        return FALSE;
    }

    // check every chunk
    while (offset <= length && length - offset >= 12)
    {
        // read the chunk length
        chunk = _emojis_internal__read_be32(bytes + offset);
        if (chunk > length - offset - 12)
        {
            return FALSE;
        }

        // calculate the checksum
        crc = 0xffffffffU;
        for (i = offset + 4; i < offset + 8 + chunk; i++)
        {
            crc ^= bytes[i];
            for (bit = 0; bit < 8; bit++)
            {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
            }
        }

        // compare the checksum
        if ((crc ^ 0xffffffffU) != _emojis_internal__read_be32(bytes + offset + 8 + chunk))
        {
            return FALSE;
        }

        // check the image header
        if (offset == 8)
        {
            if (chunk != 13 || clib__memcmp(bytes + offset + 4, "IHDR", 4) != 0)
            {
                return FALSE;
            }

            // check depth and color
            depth = bytes[offset + 16];
            color = bytes[offset + 17];
            valid_depth = (boole)((color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) || ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16)) || (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)));
            if (!valid_depth)
            {
                return FALSE;
            }

            // check the image size
            width = _emojis_internal__read_be32(bytes + offset + 8);
            height = _emojis_internal__read_be32(bytes + offset + 12);
            if (width == 0 || width > 128 || height == 0 || height > 128 || bytes[offset + 18] != 0 || bytes[offset + 19] != 0 || bytes[offset + 20] > 1)
            {
                return FALSE;
            }
        }
        else if (clib__memcmp(bytes + offset + 4, "IHDR", 4) == 0)
        {
            return FALSE;
        }

        // refuse an animated PNG
        if (clib__memcmp(bytes + offset + 4, "acTL", 4) == 0)
        {
            return FALSE;
        }

        // mark data as found
        if (clib__memcmp(bytes + offset + 4, "IDAT", 4) == 0 && chunk > 0)
        {
            has_data = TRUE;
        }

        // check the last chunk
        if (clib__memcmp(bytes + offset + 4, "IEND", 4) == 0)
        {
            return has_data && chunk == 0 && offset + 12 == length;
        }

        // move to next chunk
        offset += (uint64)chunk + 12;
    }
    return FALSE;
}

/**
 * @brief Checks JPEG frame dimensions and complete segment boundaries before image data.
 * @param bytes Decoded JPEG file.
 * @param length Number of bytes available.
 */
static boole _emojis_internal__jpeg_valid(const ubyte *bytes, uint64 length)
{
    uint64 offset = 2;
    uint64 marker = 0;
    uint64 segment = 0;
    uint64 width = 0;
    uint64 height = 0;
    boole has_frame = FALSE;

    // check start and end
    if (length < 4 || bytes[0] != 0xff || bytes[1] != 0xd8 || bytes[length - 2] != 0xff || bytes[length - 1] != 0xd9)
    {
        return FALSE;
    }

    // read every segment
    while (offset < length)
    {
        // check the marker byte
        if (bytes[offset] != 0xff)
        {
            return FALSE;
        }

        // read the marker code
        while (offset < length && bytes[offset] == 0xff)
        {
            offset++;
        }
        if (offset >= length)
        {
            return FALSE;
        }
        marker = bytes[offset++];

        // refuse wrong markers
        if (marker == 0 || marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7) || length - offset < 2)
        {
            return FALSE;
        }

        // read the segment length
        segment = ((uint64)bytes[offset] << 8) | bytes[offset + 1];
        if (segment < 2 || segment > length - offset)
        {
            return FALSE;
        }

        // check the frame header
        if (marker == 0xc0 || marker == 0xc2)
        {
            if (has_frame || segment < 8 || bytes[offset + 2] != 8 || bytes[offset + 7] == 0 || segment != 8 + 3 * (uint64)bytes[offset + 7])
            {
                return FALSE;
            }
            height = ((uint64)bytes[offset + 3] << 8) | bytes[offset + 4];
            width = ((uint64)bytes[offset + 5] << 8) | bytes[offset + 6];
            if (width == 0 || width > 128 || height == 0 || height > 128)
            {
                return FALSE;
            }
            has_frame = TRUE;
        }

        // stop at image data
        if (marker == 0xda)
        {
            return has_frame && segment >= 6 && offset + segment < length - 2;
        }

        // move to next segment
        offset += segment;
    }
    return FALSE;
}

/**
 * @brief Checks that every GIF data block fits inside the file.
 * @param bytes Decoded GIF file.
 * @param length Number of bytes available.
 * @param offset First block size on entry and position after the last block on return.
 */
static boole _emojis_internal__gif_blocks_valid(const ubyte *bytes, uint64 length, uint64 *offset)
{
    uint64 block = 0;

    // read each block size
    while (*offset < length)
    {
        block = bytes[(*offset)++];
        if (block == 0)
        {
            return TRUE;
        }
        if (block > length - *offset)
        {
            return FALSE;
        }
        *offset += block;
    }
    return FALSE;
}

/**
 * @brief Checks GIF dimensions, frame count and block lengths without changing its animation.
 * @param bytes Decoded GIF file.
 * @param length Number of bytes available.
 */
static boole _emojis_internal__gif_valid(const ubyte *bytes, uint64 length)
{
    uint64 offset = 13;
    uint64 width = 0;
    uint64 height = 0;
    uint64 left = 0;
    uint64 top = 0;
    uint64 frame_width = 0;
    uint64 frame_height = 0;
    uint64 packed = 0;
    uint64 colors = 0;
    uint64 frames = 0;
    uint64 block = 0;
    boole has_global_colors = FALSE;

    // check the GIF signature
    if (length < 14 || (clib__memcmp(bytes, "GIF87a", 6) != 0 && clib__memcmp(bytes, "GIF89a", 6) != 0))
    {
        return FALSE;
    }

    // check the image size
    width = bytes[6] | ((uint64)bytes[7] << 8);
    height = bytes[8] | ((uint64)bytes[9] << 8);
    if (width == 0 || width > 128 || height == 0 || height > 128)
    {
        return FALSE;
    }

    // skip the color table
    has_global_colors = (boole)((bytes[10] & 0x80) != 0);
    if (has_global_colors)
    {
        colors = 3U << ((bytes[10] & 7) + 1);
        if (colors > length - offset)
        {
            return FALSE;
        }
        offset += colors;
    }

    // read every block
    while (offset < length)
    {
        // read the block type
        block = bytes[offset++];

        // check the file end
        if (block == 0x3b)
        {
            return frames > 0 && offset == length;
        }

        // check an extension block
        if (block == 0x21)
        {
            // read the extension type
            if (offset >= length)
            {
                return FALSE;
            }
            block = bytes[offset++];

            // check the graphic control
            if (block == 0xf9)
            {
                if (length - offset < 6 || bytes[offset] != 4 || bytes[offset + 5] != 0 || (bytes[offset + 1] & 0xe0) != 0 || ((bytes[offset + 1] >> 2) & 7) > 3)
                {
                    return FALSE;
                }
                offset += 6;
                continue;
            }

            // check application and comment
            if (block == 0xff)
            {
                if (length - offset < 12 || bytes[offset] != 11)
                {
                    return FALSE;
                }
                offset += 12;
            }
            else if (block != 0xfe)
            {
                return FALSE;
            }

            // check the data blocks
            if (!_emojis_internal__gif_blocks_valid(bytes, length, &offset))
            {
                return FALSE;
            }
            continue;
        }

        // count the next frame
        if (block != 0x2c || length - offset < 9 || ++frames > 200)
        {
            return FALSE;
        }

        // read the frame header
        left = bytes[offset] | ((uint64)bytes[offset + 1] << 8);
        top = bytes[offset + 2] | ((uint64)bytes[offset + 3] << 8);
        frame_width = bytes[offset + 4] | ((uint64)bytes[offset + 5] << 8);
        frame_height = bytes[offset + 6] | ((uint64)bytes[offset + 7] << 8);
        packed = bytes[offset + 8];
        offset += 9;

        // check the frame limits
        if (frame_width == 0 || frame_height == 0 || left + frame_width > width || top + frame_height > height || (packed & 0x18) != 0 || (!has_global_colors && (packed & 0x80) == 0))
        {
            return FALSE;
        }

        // skip the frame colors
        if ((packed & 0x80) != 0)
        {
            colors = 3U << ((packed & 7) + 1);
            if (colors > length - offset)
            {
                return FALSE;
            }
            offset += colors;
        }

        // check the code size
        if (length - offset < 2 || bytes[offset] < 2 || bytes[offset] > 8 || bytes[offset + 1] == 0)
        {
            return FALSE;
        }
        offset++;

        // check the data blocks
        if (!_emojis_internal__gif_blocks_valid(bytes, length, &offset))
        {
            return FALSE;
        }
    }
    return FALSE;
}

/**
 * @brief Checks the image data URL, decoded size and file structure before storing it.
 * @param base64 Image data URL supplied by an administrator or saved settings.
 */
static boole _emojis_internal__image_valid(cstring base64)
{
    cstring alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    cstring payload = NULL_POINTER;
    cstring symbol = NULL_POINTER;
    ubyte *bytes = NULL_POINTER;
    uint64 length = 0;
    uint64 maximum_bytes = SERVER_EMOJI_MAX_BYTES;
    uint64 decoded = 0;
    size_t written = 0; // mbedTLS writes through a size_t pointer
    uint64 i = 0;
    uint64 padding = 0;
    uint64 last = 0;
    boole png = FALSE;
    boole gif = FALSE;
    boole valid = FALSE;

    // refuse a missing image
    if (base64 == NULL_POINTER)
    {
        return FALSE;
    }

    // read the image type
    if (clib__strncmp(base64, "data:image/png;base64,", 22) == 0)
    {
        payload = base64 + 22;
        png = TRUE;
    }
    else if (clib__strncmp(base64, "data:image/jpeg;base64,", 23) == 0)
    {
        payload = base64 + 23;
    }
    else if (clib__strncmp(base64, "data:image/gif;base64,", 22) == 0)
    {
        payload = base64 + 22;
        maximum_bytes = SERVER_EMOJI_GIF_MAX_BYTES;
        gif = TRUE;
    }
    else
    {
        return FALSE;
    }

    // check the payload length
    while (length <= 4 * ((maximum_bytes + 2) / 3) && payload[length] != 0)
    {
        length++;
    }
    if (length < 4 || length > 4 * ((maximum_bytes + 2) / 3) || length % 4 != 0)
    {
        return FALSE;
    }

    // count the padding
    if (payload[length - 1] == '=')
    {
        padding++;
    }
    if (payload[length - 2] == '=')
    {
        padding++;
    }

    // check every symbol
    for (i = 0; i < length - padding; i++)
    {
        symbol = clib__strchr(alphabet, payload[i]);
        if (symbol == NULL_POINTER)
        {
            return FALSE;
        }
        last = (uint64)(symbol - alphabet);
    }

    // check the padding bits
    if ((padding == 1 && (last & 3) != 0) || (padding == 2 && (last & 15) != 0))
    {
        return FALSE;
    }

    // check the decoded size
    decoded = (uint64)(length / 4 * 3 - padding);
    if (decoded == 0 || decoded > maximum_bytes)
    {
        return FALSE;
    }

    // allocate the image buffer
    bytes = (ubyte *)malloc(decoded);
    if (bytes == NULL_POINTER)
    {
        return FALSE;
    }

    // decode and check image
    if (mbedtls_base64_decode(bytes, decoded, &written, (const ubyte *)payload, (uint64)length) == 0 && written == decoded)
    {
        if (gif)
        {
            valid = _emojis_internal__gif_valid(bytes, decoded);
        }
        else
        {
            valid = png ? _emojis_internal__png_valid(bytes, decoded) : _emojis_internal__jpeg_valid(bytes, decoded);
        }
    }

    // free the buffer
    free(bytes);
    return valid;
}

/**
 * @brief Checks whether saved text is one of the supplied default emojis.
 * @param unicode Emoji text read from saved settings.
 */
static boole _emojis_internal__unicode_valid(cstring unicode)
{
    uint64 i = 0;

    // check the argument
    if (unicode == NULL_POINTER)
    {
        return FALSE;
    }

    // search the default emojis
    for (i = 0; i < sizeof(default_emojis) / sizeof(default_emojis[0]); i++)
    {
        if (clib__strcmp(default_emojis[i].unicode, unicode) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Creates an identifier that stays with the emoji when settings are saved.
 * @param id Output buffer with room for 33 characters.
 */
static boole _emojis_internal__new_id(char *id)
{
    static const char hex[] = "0123456789abcdef";
    ubyte random[16] = { 0 };
    uint64 i = 0;

    // create random bytes
    if (util__fill_secure_random_bytes(random, sizeof(random)) == FALSE)
    {
        return FALSE;
    }

    // convert to hex text
    for (i = 0; i < sizeof(random); i++)
    {
        id[i * 2] = hex[random[i] >> 4];
        id[i * 2 + 1] = hex[random[i] & 15];
    }
    id[32] = 0;
    return TRUE;
}

/**
 * @brief Stores an emoji under a unique name and identifier.
 * @param name Emoji name.
 * @param base64 Image data URL or NULL for a text emoji.
 * @param unicode Default emoji text or NULL for an image.
 * @param id Stable identifier that will be saved with the emoji.
 * @param enforce_budget New uploads obey the aggregate budget; existing saved images still restore.
 * @return Error description or NULL on success.
 */
static cstring _emojis_internal__store(cstring name, cstring base64, cstring unicode, cstring id, boole enforce_budget)
{
    uint64 i = 0;
    uint64 available = MAX_SERVER_EMOJIS;
    char *copy = NULL_POINTER;
    cstring error = NULL_POINTER;
    uint64 image_bytes = 0;

    // check the name
    if (!_emojis_internal__name_valid(name))
    {
        return "Use one to 32 lowercase letters, digits or underscores for the name.";
    }

    // check id and content
    if (!emojis__is_id_valid(id) || (base64 == NULL_POINTER) == (unicode == NULL_POINTER) || (unicode != NULL_POINTER && !_emojis_internal__unicode_valid(unicode)))
    {
        return "This emoji record is invalid.";
    }

    // check the image
    if (base64 != NULL_POINTER && !_emojis_internal__image_valid(base64))
    {
        return "Choose an image up to 128 by 128 pixels. PNG and JPEG can use 30000 bytes. GIF can use 256 KiB and 200 frames.";
    }

    // count the image bytes
    if (base64 != NULL_POINTER)
    {
        image_bytes = (uint64)clib__strlen(base64);
    }

    // lock the emojis
    pthread_mutex_lock(&server_emojis_mutex);

    // find slot, check duplicates
    for (i = 0; i < MAX_SERVER_EMOJIS; i++)
    {
        if (server_emojis[i].id[0] == 0)
        {
            if (available == MAX_SERVER_EMOJIS)
            {
                available = i;
            }
        }
        else if (clib__strcmp(server_emojis[i].name, name) == 0 || clib__strcmp(server_emojis[i].id, id) == 0)
        {
            error = "An emoji with that name or identifier already exists.";
            break;
        }
    }

    // refuse a full list
    if (error == NULL_POINTER && available == MAX_SERVER_EMOJIS)
    {
        error = "This server already has 256 emojis.";
    }

    // check the storage limit
    if (error == NULL_POINTER && enforce_budget && (image_bytes > SERVER_EMOJI_STORAGE_MAX_BYTES || server_emoji_image_bytes > SERVER_EMOJI_STORAGE_MAX_BYTES - image_bytes))
    {
        error = "Emoji images have a combined 32 MiB encoded storage limit. Delete some images before uploading another.";
    }

    // store the emoji
    if (error == NULL_POINTER)
    {
        // allocate the image copy
        if (base64 != NULL_POINTER)
        {
            copy = (char *)malloc((uint64)image_bytes + 1);
        }
        if (base64 != NULL_POINTER && copy == NULL_POINTER)
        {
            error = "There is not enough memory to store this emoji.";
        }
        else
        {
            // fill the emoji slot
            if (base64 != NULL_POINTER)
            {
                clib__string_copy(copy, image_bytes + 1, base64);
            }
            if (unicode != NULL_POINTER)
            {
                clib__string_copy(server_emojis[available].unicode, sizeof(server_emojis[available].unicode), unicode);
            }
            clib__string_copy(server_emojis[available].id, sizeof(server_emojis[available].id), id);
            clib__string_copy(server_emojis[available].name, sizeof(server_emojis[available].name), name);
            server_emojis[available].base64 = copy;
            server_emoji_image_bytes += image_bytes;
        }
    }

    // unlock the emojis
    pthread_mutex_unlock(&server_emojis_mutex);
    return error;
}

/**
 * @brief Creates a stable random identifier and adds an uploaded emoji.
 * @param name Unique lowercase emoji name.
 * @param base64 PNG, JPEG or GIF data URL.
 * @param id Output buffer with room for 33 characters.
 * @return Error description or NULL on success.
 */
cstring emojis__add(cstring name, cstring base64, char *id)
{
    // create the identifier
    if (_emojis_internal__new_id(id) == FALSE)
    {
        return "Could not create an emoji identifier.";
    }

    // store the emoji
    return _emojis_internal__store(name, base64, NULL_POINTER, id, TRUE);
}

/**
 * @brief Deletes one emoji without changing other emoji identifiers.
 * @param id Identifier supplied by the administrator.
 */
boole emojis__remove(cstring id)
{
    uint64 i = 0;
    boole removed = FALSE;

    // check the id
    if (!emojis__is_id_valid(id))
    {
        return FALSE;
    }

    // remove the matching emoji
    pthread_mutex_lock(&server_emojis_mutex);
    for (i = 0; i < MAX_SERVER_EMOJIS; i++)
    {
        if (server_emojis[i].id[0] != 0 && clib__strcmp(server_emojis[i].id, id) == 0)
        {
            if (server_emojis[i].base64 != NULL_POINTER)
            {
                server_emoji_image_bytes -= (uint64)clib__strlen(server_emojis[i].base64);
            }
            free(server_emojis[i].base64);
            clib__memset(&server_emojis[i], 0, sizeof(server_emojis[i]));
            removed = TRUE;
            break;
        }
    }
    pthread_mutex_unlock(&server_emojis_mutex);
    return removed;
}

/**
 * @brief Copies the emoji list for a message or saved settings.
 * @param include_images TRUE includes image data for saving.
 */
static cJSON *_emojis_internal__array(boole include_images)
{
    cJSON *array = cJSON_CreateArray();
    cJSON *entry = NULL_POINTER;
    uint64 i = 0;

    // check the new array
    if (array == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    // copy every emoji
    pthread_mutex_lock(&server_emojis_mutex);
    for (i = 0; i < MAX_SERVER_EMOJIS; i++)
    {
        // skip empty slots
        if (server_emojis[i].id[0] == 0)
        {
            continue;
        }

        // build the emoji entry
        entry = cJSON_CreateObject();
        if (entry == NULL_POINTER || cJSON_AddStringToObject(entry, "id", server_emojis[i].id) == NULL_POINTER || cJSON_AddStringToObject(entry, "name", server_emojis[i].name) == NULL_POINTER || (server_emojis[i].unicode[0] != 0 && cJSON_AddStringToObject(entry, "unicode", server_emojis[i].unicode) == NULL_POINTER) || (include_images && server_emojis[i].base64 != NULL_POINTER && cJSON_AddStringToObject(entry, "base64", server_emojis[i].base64) == NULL_POINTER) || !cJSON_AddItemToArray(array, entry))
        {
            cJSON_Delete(entry);
            cJSON_Delete(array);
            array = NULL_POINTER;
            break;
        }
    }
    pthread_mutex_unlock(&server_emojis_mutex);
    return array;
}

/** @brief Copies the names and text emojis without sending uploaded images at login. */
cJSON *emojis__catalogue(void)
{
    return _emojis_internal__array(FALSE);
}

/** @brief Copies complete emoji records for server_settings.json. */
cJSON *emojis__write(void)
{
    return _emojis_internal__array(TRUE);
}

/**
 * @brief Copies a requested image so sending it does not hold the emoji mutex.
 * @param id Identifier requested by a connected client.
 */
cJSON *emojis__image(cstring id)
{
    cJSON *image = NULL_POINTER;
    uint64 i = 0;

    // check the id
    if (!emojis__is_id_valid(id))
    {
        return NULL_POINTER;
    }

    // copy the matching image
    pthread_mutex_lock(&server_emojis_mutex);
    for (i = 0; i < MAX_SERVER_EMOJIS; i++)
    {
        if (server_emojis[i].base64 != NULL_POINTER && clib__strcmp(server_emojis[i].id, id) == 0)
        {
            image = cJSON_CreateObject();
            if (image == NULL_POINTER || cJSON_AddStringToObject(image, "emoji_id", id) == NULL_POINTER || cJSON_AddStringToObject(image, "name", server_emojis[i].name) == NULL_POINTER || cJSON_AddStringToObject(image, "base64", server_emojis[i].base64) == NULL_POINTER)
            {
                cJSON_Delete(image);
                image = NULL_POINTER;
            }
            break;
        }
    }
    pthread_mutex_unlock(&server_emojis_mutex);
    return image;
}

/**
 * @brief Restores saved emojis or adds the defaults when no list has been saved.
 * @param array Saved emoji list or NULL when the settings have no emojis member.
 */
void emojis__load(cJSON *array)
{
    cJSON *entry = NULL_POINTER;
    cJSON *id = NULL_POINTER;
    cJSON *name = NULL_POINTER;
    cJSON *base64 = NULL_POINTER;
    cJSON *unicode = NULL_POINTER;
    static const char hex[] = "0123456789abcdef";
    char new_id[33] = "1e000000000000000000000000000000";
    uint64 i = 0;

    // free all emojis
    pthread_mutex_lock(&server_emojis_mutex);
    server_emoji_image_bytes = 0;
    for (i = 0; i < MAX_SERVER_EMOJIS; i++)
    {
        free(server_emojis[i].base64);
        clib__memset(&server_emojis[i], 0, sizeof(server_emojis[i]));
    }
    pthread_mutex_unlock(&server_emojis_mutex);

    // add the default emojis
    if (array == NULL_POINTER)
    {
        for (i = 0; i < sizeof(default_emojis) / sizeof(default_emojis[0]); i++)
        {
            new_id[30] = hex[(i + 1) >> 4];
            new_id[31] = hex[(i + 1) & 15];
            _emojis_internal__store(default_emojis[i].name, NULL_POINTER, default_emojis[i].unicode, new_id, FALSE);
        }
        return;
    }

    // check the saved list
    if (!cJSON_IsArray(array))
    {
        return;
    }

    // load every saved emoji
    cJSON_ArrayForEach(entry, array)
    {
        id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        base64 = cJSON_GetObjectItemCaseSensitive(entry, "base64");
        unicode = cJSON_GetObjectItemCaseSensitive(entry, "unicode");
        if (cJSON_IsString(id) && cJSON_IsString(name) && ((cJSON_IsString(base64) && unicode == NULL_POINTER) || (cJSON_IsString(unicode) && base64 == NULL_POINTER)))
        {
            _emojis_internal__store(name->valuestring, cJSON_IsString(base64) ? base64->valuestring : NULL_POINTER, cJSON_IsString(unicode) ? unicode->valuestring : NULL_POINTER, id->valuestring, FALSE);
        }
    }
}
