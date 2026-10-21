// This file manages bans, tags and icons, checks banner images and limits attempts to guess the admin password.
#include "identity_store.h"
#include "administration.h"
#include "../definitions.h"
#include "../../third-party/dave-g-json/cJSON.h"
#include "../../third-party/rxi-log/log.h"
#include "../../third-party/eteran-cvector/cvector.h"
#include "../base.h"

#include "../memory_manager.h"
#include "../clib/clib_memory.h"
#include "../clib/clib_string.h"
#include "../util.h"
#include "audio_channel.h"
#include "../audio_websocket.h"
#include "../video_websocket.h"
#include "video_channel.h"
#include "server_logs.h"
#include "server_settings.h"
#include "login_admission.h"
#include "../../third-party/zhicheng/base64.h"
#include "../../third-party/mbedtls-3.6.6/include/mbedtls/base64.h"

#define ADMIN_PASSWORD_RATE_BUCKETS 1024
#define ADMIN_PASSWORD_ATTEMPTS_PER_MINUTE 5
#define ADMIN_PASSWORD_ATTEMPT_WINDOW_MS 60000

typedef struct admin_password_attempts_t
{
    char ip_address[INET6_ADDRSTRLEN];
    uint64 window_started_ms;
    uint attempts;
} admin_password_attempts_t;

static admin_password_attempts_t* g_admin_password_attempts = NULL_POINTER;

// declarations
static boole _server_admin_internal__is_png_banner_header_valid(const ubyte* image, size_t length);
static boole _server_admin_internal__is_jpeg_banner_header_valid(const ubyte* image, size_t length);
static boole _server_admin_internal__is_webp_banner_header_valid(const ubyte* image, size_t length);

/**
 * @brief checks whether an ip address is currently banned
 *
 * @param char* ip_address -> the ip to check
 *
 * @return boole TRUE if the ip is in the ban list
 */
boole server_admin__is_ip_banned(char* ip_address)
{
    boole is_banned = FALSE;
    uint64 i = 0;

    if (ip_address == NULL_POINTER || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    clib__read_lock(&g_bans_global_rwlock_guard);
    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == FALSE)
        {
            continue;
        }
        if (clib__is_string_equal(g_ban_array[i].ip_address, ip_address) == TRUE)
        {
            is_banned = TRUE;
            break;
        }
    }
    clib__unlock(&g_bans_global_rwlock_guard);

    return is_banned;
}

/**
 * @brief whether a public key is on the ban list. a ban records the target's identity at ban
 *        time, so the same identity is refused even when it returns from a different ip address.
 *        entries without a recorded identity never match
 *
 * @param char* public_key -> the connecting client's public key
 *
 * @return boole TRUE if the identity is in the ban list
 */
boole server_admin__is_identity_banned(char* public_key)
{
    boole is_banned = FALSE;
    uint64 i = 0;

    if (public_key == NULL_POINTER || public_key[0] == 0 || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    clib__read_lock(&g_bans_global_rwlock_guard);
    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == FALSE || g_ban_array[i].identity[0] == 0)
        {
            continue;
        }
        if (clib__is_string_equal(g_ban_array[i].identity, public_key) == TRUE)
        {
            is_banned = TRUE;
            break;
        }
    }
    clib__unlock(&g_bans_global_rwlock_guard);

    return is_banned;
}

/**
 * @brief whether a resolved country code is on the join block list. an empty or unresolved
 *        country (local addresses, lookup failures) is never blocked
 *
 * @param char* country_iso_code -> uppercase 2-letter code as the geoip db emits it
 *
 * @return boole TRUE when country blocking is on and the code is listed
 */
boole server_admin__is_country_blocked(char* country_iso_code)
{
    uint64 i = 0;

    if (g_server_settings.is_country_blocking_active == FALSE || country_iso_code == NULL_POINTER || country_iso_code[0] == 0)
    {
        return FALSE;
    }

    for (i = 0; i < g_server_settings.blocked_countries_count; i++)
    {
        if (clib__is_string_equal(&g_server_settings.blocked_countries[i][0], country_iso_code) == TRUE)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief adds a ban entry. ignored if the ip is already banned or the ban list is full
 *
 * @param char* ip_address -> the banned ip (matched at socket open)
 * @param char* country_iso_code -> the client's country at ban time (may be empty)
 * @param char* identity -> the client's identity / public key at ban time (matched at join; may be empty)
 * @param char* extra_data -> free-form extra data, e.g. a future fingerprint (may be empty)
 *
 * @attention caller must hold the bans write lock
 *
 * @return boole TRUE if a ban was added
 */
boole server_admin__add_ban(char* ip_address, char* country_iso_code, char* identity, char* extra_data)
{
    uint64 i = 0;
    int64 free_index = -1;

    if (ip_address == NULL_POINTER || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == TRUE)
        {
            if (clib__is_string_equal(g_ban_array[i].ip_address, ip_address) == TRUE)
            {
                return FALSE; // already banned
            }
        }
        else if (free_index == -1)
        {
            free_index = (int64)i;
        }
    }

    if (free_index == -1)
    {
        log_info("%s", "server_admin__add_ban: ban list is full");
        return FALSE;
    }

    clib__null_memory(&g_ban_array[free_index], sizeof(ban_entry_t));
    g_ban_array[free_index].is_existing = TRUE;
    g_ban_array[free_index].timestamp_banned = util__get_timestamp_ms();
    clib__copy_memory(ip_address, &g_ban_array[free_index].ip_address[0], clib__utf8_string_length(ip_address), BAN_IP_MAX_LENGTH - 1);
    if (country_iso_code != NULL_POINTER)
    {
        clib__copy_memory(country_iso_code, &g_ban_array[free_index].country_iso_code[0], clib__utf8_string_length(country_iso_code), COUNTRY_ISO_CODE_LENGTH - 1);
    }
    if (identity != NULL_POINTER)
    {
        clib__copy_memory(identity, &g_ban_array[free_index].identity[0], clib__utf8_string_length(identity), MAX_PUBLIC_KEY_LENGTH - 1);
    }
    if (extra_data != NULL_POINTER)
    {
        clib__copy_memory(extra_data, &g_ban_array[free_index].extra_data[0], clib__utf8_string_length(extra_data), BAN_EXTRA_DATA_MAX_LENGTH - 1);
    }

    return TRUE;
}

/**
 * @brief removes a ban entry matching an ip address
 *
 * @param char* ip_address -> the ip to unban
 *
 * @attention caller must hold the bans write lock
 *
 * @return boole TRUE if an entry was removed
 */
boole server_admin__remove_ban_by_ip(char* ip_address)
{
    uint64 i = 0;

    if (ip_address == NULL_POINTER || g_ban_array == NULL_POINTER)
    {
        return FALSE;
    }

    for (i = 0; i < MAX_BANS; i++)
    {
        if (g_ban_array[i].is_existing == TRUE && clib__is_string_equal(&g_ban_array[i].ip_address[0], ip_address) == TRUE)
        {
            clib__null_memory(&g_ban_array[i], sizeof(ban_entry_t));
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief returns TRUE if a tag with the given id exists
 *
 * @param uint64 tag_id -> id of the tag to check
 *
 * @return boole
 */
boole server_admin__is_tag_id_real(uint64 tag_id)
{
    boole result = FALSE;
    uint64 i = 0;

    result = FALSE;

    for (i = 0; i < MAX_TAGS; i++)
    {
        if (g_tags_array[i].is_existing == FALSE)
        {
            continue;
        }

        if (g_tags_array[i].id == tag_id)
        {
            result = TRUE;
            break;
        }
    }

    return result;
}

/**
 * @brief returns TRUE if the client already has the given tag id
 *
 * @param uint64 client_id -> id of the client
 * @param uint64 this_tag_id -> id of the tag
 *
 * @return boole
 *
 * @attention this function assumes that client_id is correct and that this_tag_id is also correct
 */
boole server_admin__is_client_already_assigned_this_tag_id(uint64 client_id, uint64 this_tag_id)
{
    uint64 cvector_loop_index = 0;
    client_t* client = NULL_POINTER;
    boole is_tag_found = FALSE;

    client = &g_clients_array[client_id];

    if (client->tag_ids != NULL_POINTER)
    {
        for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->tag_ids); ++cvector_loop_index)
        {
            if (client->tag_ids[cvector_loop_index] == this_tag_id)
            {
                is_tag_found = TRUE;
                break;
            }
        }
    }

    return is_tag_found;
}

/**
 * @brief returns the index of the given tag id within the client's tag vector, or -1 if not found
 *
 * @param uint64 client_id -> id of the client
 * @param uint64 this_tag_id -> id of the tag
 *
 * @return int64 index of the tag id, or -1 if the client does not have it
 *
 * @attention this function assumes that client_id is correct and that this_tag_id is also correct
 */
int64 server_admin__get_index_of_tag_id_of_client(uint64 client_id, uint64 this_tag_id)
{
    uint64 cvector_loop_index = 0;
    client_t* client = NULL_POINTER;
    int64 tag_index = -1;

    client = &g_clients_array[client_id];

    if (client->tag_ids != NULL_POINTER)
    {
        for (cvector_loop_index = 0; cvector_loop_index < cvector_size(client->tag_ids); ++cvector_loop_index)
        {
            if (client->tag_ids[cvector_loop_index] == this_tag_id)
            {
                tag_index = cvector_loop_index;
                break;
            }
        }
    }

    return tag_index;
}

/**
 * @brief Checks the PNG signature, first image header and banner dimensions.
 * @param const ubyte* image -> decoded image bytes
 * @param size_t length -> available image bytes
 * @return boole -> TRUE when the header describes an allowed banner size
 */
static boole _server_admin_internal__is_png_banner_header_valid(const ubyte* image, size_t length)
{
    static const ubyte signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint width = 0;
    uint height = 0;

    if (length < 33 || memcmp(image, signature, sizeof(signature)) != 0 || image[8] != 0 || image[9] != 0 || image[10] != 0 || image[11] != 13 || memcmp(image + 12, "IHDR", 4) != 0)
    {
        return FALSE;
    }
    width = ((uint)image[16] << 24) | ((uint)image[17] << 16) | ((uint)image[18] << 8) | image[19];
    height = ((uint)image[20] << 24) | ((uint)image[21] << 16) | ((uint)image[22] << 8) | image[23];
    return width >= 1 && width <= 2048 && height >= 1 && height <= 512;
}

/**
 * @brief Finds JPEG image dimensions without reading beyond any marker segment.
 * @param const ubyte* image -> decoded image bytes
 * @param size_t length -> available image bytes
 * @return boole -> TRUE when a complete frame header gives an allowed banner size
 */
static boole _server_admin_internal__is_jpeg_banner_header_valid(const ubyte* image, size_t length)
{
    size_t offset = 2;
    uint marker = 0;
    uint segment_length = 0;
    uint width = 0;
    uint height = 0;

    if (length < 4 || image[0] != 0xff || image[1] != 0xd8 || image[length - 2] != 0xff || image[length - 1] != 0xd9)
    {
        return FALSE;
    }
    while (offset < length)
    {
        if (image[offset] != 0xff)
        {
            return FALSE;
        }
        while (offset < length && image[offset] == 0xff)
        {
            offset++;
        }
        if (offset >= length)
        {
            return FALSE;
        }
        marker = image[offset++];
        if (marker == 0x01)
        {
            continue;
        }
        if (marker == 0x00 || (marker >= 0xd0 && marker <= 0xda) || length - offset < 2)
        {
            return FALSE;
        }
        segment_length = ((uint)image[offset] << 8) | image[offset + 1];
        if (segment_length < 2 || segment_length > length - offset)
        {
            return FALSE;
        }
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) || (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf))
        {
            if (segment_length < 8 || image[offset + 7] == 0 || segment_length != 8 + 3 * (uint)image[offset + 7])
            {
                return FALSE;
            }
            height = ((uint)image[offset + 3] << 8) | image[offset + 4];
            width = ((uint)image[offset + 5] << 8) | image[offset + 6];
            return width >= 1 && width <= 2048 && height >= 1 && height <= 512;
        }
        offset += segment_length;
    }
    return FALSE;
}

/**
 * @brief Checks the WebP container and reads its image or canvas dimensions.
 * @param const ubyte* image -> decoded image bytes
 * @param size_t length -> available image bytes
 * @return boole -> TRUE when the header describes an allowed banner size
 */
static boole _server_admin_internal__is_webp_banner_header_valid(const ubyte* image, size_t length)
{
    uint container_length = 0;
    uint chunk_length = 0;
    uint width = 0;
    uint height = 0;

    if (length < 20 || memcmp(image, "RIFF", 4) != 0 || memcmp(image + 8, "WEBP", 4) != 0)
    {
        return FALSE;
    }
    container_length = (uint)image[4] | ((uint)image[5] << 8) | ((uint)image[6] << 16) | ((uint)image[7] << 24);
    chunk_length = (uint)image[16] | ((uint)image[17] << 8) | ((uint)image[18] << 16) | ((uint)image[19] << 24);
    if (container_length != length - 8 || chunk_length > length - 20 || (chunk_length & 1) > length - 20 - chunk_length)
    {
        return FALSE;
    }
    if (memcmp(image + 12, "VP8 ", 4) == 0)
    {
        if (chunk_length < 10 || (image[20] & 1) != 0 || image[23] != 0x9d || image[24] != 0x01 || image[25] != 0x2a)
        {
            return FALSE;
        }
        width = ((uint)image[26] | ((uint)image[27] << 8)) & 0x3fff;
        height = ((uint)image[28] | ((uint)image[29] << 8)) & 0x3fff;
    }
    else if (memcmp(image + 12, "VP8L", 4) == 0)
    {
        if (chunk_length < 5 || image[20] != 0x2f || (image[24] >> 5) != 0)
        {
            return FALSE;
        }
        width = 1 + (((uint)image[21] | ((uint)image[22] << 8)) & 0x3fff);
        height = 1 + ((uint)(image[22] >> 6) | ((uint)image[23] << 2) | ((uint)(image[24] & 0x0f) << 10));
    }
    else if (memcmp(image + 12, "VP8X", 4) == 0)
    {
        if (chunk_length != 10 || (image[20] & 0xc1) != 0 || image[21] != 0 || image[22] != 0 || image[23] != 0)
        {
            return FALSE;
        }
        width = 1 + ((uint)image[24] | ((uint)image[25] << 8) | ((uint)image[26] << 16));
        height = 1 + ((uint)image[27] | ((uint)image[28] << 8) | ((uint)image[29] << 16));
    }
    else
    {
        return FALSE;
    }
    return width >= 1 && width <= 2048 && height >= 1 && height <= 512;
}

/**
 * @brief Checks an uploaded banner's data URL, size, image header and dimensions.
 * @param cstring banner -> PNG, JPEG or WebP data URL; an empty string removes it
 * @return boole -> TRUE for an empty string or a supported image header within the limits
 */
boole server_admin__is_server_banner_valid(cstring banner)
{
    cstring alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    cstring payload = NULL_POINTER;
    cstring symbol = NULL_POINTER;
    ubyte* image = NULL_POINTER;
    int64 banner_length = 0;
    size_t prefix_length = 0;
    size_t payload_length = 0;
    size_t decoded_length = 0;
    size_t bytes_written = 0;
    size_t index = 0;
    uint image_type = 0;
    uint padding = 0;
    uint last_value = 0;
    boole valid = FALSE;

    if (banner == NULL_POINTER)
    {
        return FALSE;
    }
    if (banner[0] == 0)
    {
        return TRUE;
    }
    banner_length = clib__utf8_string_length_check_max_length(banner, sizeof("data:image/jpeg;base64,") - 1 + 4 * ((SERVER_BANNER_MAX_BYTES + 2) / 3));
    if (banner_length < 0)
    {
        return FALSE;
    }
    if (strncmp(banner, "data:image/png;base64,", sizeof("data:image/png;base64,") - 1) == 0)
    {
        prefix_length = sizeof("data:image/png;base64,") - 1;
        image_type = 1;
    }
    else if (strncmp(banner, "data:image/jpeg;base64,", sizeof("data:image/jpeg;base64,") - 1) == 0)
    {
        prefix_length = sizeof("data:image/jpeg;base64,") - 1;
        image_type = 2;
    }
    else if (strncmp(banner, "data:image/webp;base64,", sizeof("data:image/webp;base64,") - 1) == 0)
    {
        prefix_length = sizeof("data:image/webp;base64,") - 1;
        image_type = 3;
    }
    else
    {
        return FALSE;
    }
    payload = banner + prefix_length;
    payload_length = (size_t)banner_length - prefix_length;
    if (payload_length < 4 || payload_length % 4 != 0)
    {
        return FALSE;
    }
    if (payload[payload_length - 1] == '=')
    {
        padding++;
    }
    if (payload[payload_length - 2] == '=')
    {
        padding++;
    }
    for (index = 0; index < payload_length - padding; index++)
    {
        symbol = strchr(alphabet, payload[index]);
        if (symbol == NULL_POINTER)
        {
            return FALSE;
        }
        last_value = (uint)(symbol - alphabet);
    }
    if ((padding == 1 && (last_value & 3) != 0) || (padding == 2 && (last_value & 15) != 0))
    {
        return FALSE;
    }
    decoded_length = payload_length / 4 * 3 - padding;
    if (decoded_length > SERVER_BANNER_MAX_BYTES)
    {
        return FALSE;
    }
    image = (ubyte*)malloc(decoded_length);
    if (image == NULL_POINTER)
    {
        return FALSE;
    }
    if (mbedtls_base64_decode(image, decoded_length, &bytes_written, (const ubyte*)payload, payload_length) == 0 && bytes_written == decoded_length)
    {
        if (image_type == 1)
        {
            valid = _server_admin_internal__is_png_banner_header_valid(image, bytes_written);
        }
        else if (image_type == 2)
        {
            valid = _server_admin_internal__is_jpeg_banner_header_valid(image, bytes_written);
        }
        else
        {
            valid = _server_admin_internal__is_webp_banner_header_valid(image, bytes_written);
        }
    }
    free(image);
    return valid;
}

/**
 * @brief gives an icon slot its own heap copy of a base64 data url, replacing whatever it held
 *
 * @param icon_t* icon -> the slot
 * @param cstring base64 -> the data url, cut at ICON_MAX_LENGTH - 1 characters
 *
 * @return boole -> FALSE when the allocation failed (the slot is then empty)
 */
boole server_admin__set_icon_base64(icon_t* icon, cstring base64)
{
    uint64 length = 0;

    server_admin__free_icon_base64(icon);

    length = clib__utf8_string_length(base64);
    if (length > ICON_MAX_LENGTH - 1)
    {
        length = ICON_MAX_LENGTH - 1;
    }

    icon->base64 = (char*)memorymanager__allocate(length + 1, MEMALLOC_ICON); // zeroed, so the copy ends in a NUL
    if (icon->base64 == NULL_POINTER)
    {
        return FALSE;
    }

    clib__copy_memory((void*)base64, (void*)icon->base64, length, length + 1);

    return TRUE;
}

/**
 * @brief releases an icon slot's base64 buffer, if it has one
 *
 * @param icon_t* icon -> the slot
 *
 * @return void
 */
void server_admin__free_icon_base64(icon_t* icon)
{
    if (icon->base64 != NULL_POINTER)
    {
        memorymanager__free((nuint)icon->base64);
        icon->base64 = NULL_POINTER;
    }
}


/**
 * @brief Records an admin-password attempt when the address has not used its five-per-minute limit.
 * @param char* ip_address -> address making the attempt
 * @param uint64 now -> current server timestamp in milliseconds
 * @return boole -> TRUE when allowed; FALSE when limited or tracking storage is unavailable
 * @attention The caller holds the clients write lock.
 */
boole server_admin__allow_admin_password_attempt(char* ip_address, uint64 now)
{
    admin_password_attempts_t* available = NULL_POINTER;
    uint64 i;

    if (g_admin_password_attempts == NULL_POINTER)
    {
        g_admin_password_attempts = calloc(ADMIN_PASSWORD_RATE_BUCKETS, sizeof(*g_admin_password_attempts));
        if (g_admin_password_attempts == NULL_POINTER)
        {
            return FALSE;
        }
    }
    for (i = 0; i < ADMIN_PASSWORD_RATE_BUCKETS; i++)
    {
        admin_password_attempts_t* entry = &g_admin_password_attempts[i];
        if (entry->attempts == 0 || now < entry->window_started_ms || now - entry->window_started_ms >= ADMIN_PASSWORD_ATTEMPT_WINDOW_MS)
        {
            if (available == NULL_POINTER)
            {
                available = entry;
            }
            continue;
        }
        if (strcmp(entry->ip_address, ip_address) == 0)
        {
            if (entry->attempts >= ADMIN_PASSWORD_ATTEMPTS_PER_MINUTE)
            {
                return FALSE;
            }
            entry->attempts++;
            return TRUE;
        }
    }

    /* A full table fails closed until a bucket expires; never evict a live limit. */
    if (available == NULL_POINTER)
    {
        return FALSE;
    }
    snprintf(available->ip_address, sizeof(available->ip_address), "%s", ip_address);
    available->window_started_ms = now;
    available->attempts = 1;
    return TRUE;
}


