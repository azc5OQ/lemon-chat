#include "musicbot_store.h"
#include "../crypto/ciphers.h"
#include "musicbot.h"
#include "server_settings.h"
#include "../base.h"
#include "../clib/clib.h"
#include "../crypto/crypto_storage.h"
#include "../memory_manager.h"
#include "../msg/server/server_msg.h"
#include "../util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

// A snapshot is bounded before allocation. Normal uploads and playback stay in RAM.
#define MUSICBOT_SNAPSHOT_MAX_BYTES ((uint64)1024 * 1024 * 1024)
static char g_musicbot_store_error[256];
static boole g_musicbot_store_restored = FALSE;
static boole g_musicbot_store_recovery_pending = FALSE;

typedef struct musicbot_store_buffer
{
    ubyte *data;
    uint64 size;
    uint64 position;
    boole valid;
} musicbot_store_buffer_t;

static boole _musicbot_store_internal__error(const char *text)
{
    snprintf(g_musicbot_store_error, sizeof(g_musicbot_store_error), "%s", text);
    return FALSE;
}

static boole _musicbot_store_internal__valid_name(const char *filename)
{
    uint64 index = 0;

    if (filename == NULL_POINTER || strlen(filename) != 47 || memcmp(filename, "music-bots-", 11) != 0 || strcmp(filename + 43, ".bin") != 0)
    {
        return FALSE;
    }
    for (index = 11; index < 43; index++)
    {
        if (strchr("0123456789abcdef", filename[index]) == NULL_POINTER)
        {
            return FALSE;
        }
    }
    return TRUE;
}

static boole _musicbot_store_internal__reference_may_exist(const char *filename)
{
#ifdef WIN32
    return GetFileAttributesA(filename) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND;
#else
    struct stat information = { 0 };

    return lstat(filename, &information) == 0 || errno != ENOENT;
#endif
}

boole musicbot_store__remove_file(const char *filename)
{
    FILE *file = NULL_POINTER;
    ubyte zeros[16384] = { 0 };
    int64 length = 0;
    uint64 remaining = 0;
    uint64 amount = 0;
    boole success = FALSE;
    int descriptor = -1; // Native descriptor passed to fdopen.
#ifdef WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information = { 0 };
    FILE_DISPOSITION_INFO disposition = { TRUE };
    DWORD open_error = 0;
#else
    struct stat information = { 0 };
    struct stat current = { 0 };
#endif

    if (filename[0] == 0)
    {
        return TRUE;
    }
    if (!_musicbot_store_internal__valid_name(filename))
    {
        return _musicbot_store_internal__error("The snapshot filename is invalid; no file was deleted.");
    }

    // Open only this regular file
#ifdef WIN32
    handle = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL_POINTER, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL_POINTER);
    if (handle == INVALID_HANDLE_VALUE)
    {
        open_error = GetLastError();
        return open_error == ERROR_FILE_NOT_FOUND || _musicbot_store_internal__error("The saved music could not be opened for overwriting. Check file permissions and try Clear from disk again.");
    }
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &information) || (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 || information.nNumberOfLinks != 1)
    {
        CloseHandle(handle);
        return _musicbot_store_internal__error("The saved music path is not a regular file with one link. No file was overwritten or deleted.");
    }
    descriptor = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (descriptor < 0)
    {
        CloseHandle(handle);
    }
    else
    {
        file = _fdopen(descriptor, "r+b");
        if (file == NULL_POINTER)
        {
            _close(descriptor);
        }
    }
#else
    descriptor = open(filename, O_RDWR | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
    {
        return errno == ENOENT || _musicbot_store_internal__error("The saved music could not be opened for overwriting. Check file permissions and try Clear from disk again.");
    }
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) || information.st_nlink != 1)
    {
        close(descriptor);
        return _musicbot_store_internal__error("The saved music path is not a regular file with one link. No file was overwritten or deleted.");
    }
    file = fdopen(descriptor, "r+b");
    if (file == NULL_POINTER)
    {
        close(descriptor);
    }
#endif
    if (file == NULL_POINTER)
    {
        return _musicbot_store_internal__error("The saved music could not be opened for overwriting. No file was deleted.");
    }

    // Overwrite saved bytes
    if (fseek(file, 0, SEEK_END) == 0)
    {
        length = ftell(file);
        success = length >= 0 && fseek(file, 0, SEEK_SET) == 0;
    }
    remaining = success ? (uint64)length : 0;
    while (success && remaining != 0)
    {
        amount = remaining < sizeof(zeros) ? remaining : sizeof(zeros);
        success = fwrite(zeros, 1, amount, file) == amount;
        remaining -= amount;
    }

    // Flush before deleting
    success = success && fflush(file) == 0;
#ifdef WIN32
    success = success && _commit(_fileno(file)) == 0;
#else
    success = success && fsync(fileno(file)) == 0;
#endif
    if (!success)
    {
        fclose(file);
        return _musicbot_store_internal__error("The saved music could not be fully overwritten and flushed. The file was not deleted; try Clear from disk again.");
    }

    // Remove the opened file
#ifdef WIN32
    success = SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
#else
    success = lstat(filename, &current) == 0 && current.st_dev == information.st_dev && current.st_ino == information.st_ino && remove(filename) == 0;
#endif
    if (fclose(file) != 0)
    {
        success = FALSE;
    }
    if (!success)
    {
        return _musicbot_store_internal__error("The saved music was overwritten, but clearing its file did not finish successfully. Check file permissions and try Clear from disk again.");
    }
    return TRUE;
}

static void _musicbot_store_internal__write(musicbot_store_buffer_t *buffer, const void *data, uint64 size)
{
    if (!buffer->valid || size > buffer->size - buffer->position)
    {
        buffer->valid = FALSE;
        return;
    }
    if (buffer->data != NULL_POINTER)
    {
        memcpy(buffer->data + buffer->position, data, size);
    }
    buffer->position += size;
}

static void _musicbot_store_internal__number(musicbot_store_buffer_t *buffer, uint64 value)
{
    uint64 index = 0;
    ubyte bytes[8] = { 0 };

    for (index = 0; index < 8; index++)
    {
        bytes[index] = (ubyte)(value >> (8 * index));
    }
    _musicbot_store_internal__write(buffer, bytes, sizeof(bytes));
}

static void _musicbot_store_internal__string(musicbot_store_buffer_t *buffer, const char *text)
{
    uint64 size = strlen(text);

    _musicbot_store_internal__number(buffer, size);
    _musicbot_store_internal__write(buffer, text, size);
}

static boole _musicbot_store_internal__serialize(musicbot_store_buffer_t *buffer)
{
    uint64 count_index = 0;
    client_t *count_bot = NULL_POINTER;
    uint64 index = 0;
    client_t *bot = NULL_POINTER;
    uint64 count_slot = 0;
    music_bot_single_song_data_t *count_song = NULL_POINTER;
    uint64 slot = 0;
    music_bot_single_song_data_t *song = NULL_POINTER;
    uint64 count = 0;

    for (count_index = 0; count_index < g_server_settings.max_client_count; count_index++)
    {
        count_bot = &g_clients_array[count_index];
        if (count_bot->is_existing && count_bot->is_music_bot)
        {
            if (count_bot->music_bot_client_extension.pending_song_uploads != 0)
            {
                return _musicbot_store_internal__error("A music upload is still in progress. Wait for it to finish, then save again.");
            }
            count++;
        }
    }
    _musicbot_store_internal__number(buffer, count);
    for (index = 0; index < g_server_settings.max_client_count; index++)
    {
        bot = &g_clients_array[index];
        if (!bot->is_existing || !bot->is_music_bot)
        {
            continue;
        }
        _musicbot_store_internal__number(buffer, bot->channel_id);
        _musicbot_store_internal__string(buffer, bot->username);
        _musicbot_store_internal__number(buffer, atomic_load(&bot->music_bot_client_extension.is_playback_paused));
        count = 0;
        for (count_slot = 0; count_slot < MUSIC_BOT_MAX_FILE_COUNT; count_slot++)
        {
            count_song = &bot->music_bot_client_extension.songs[count_slot];
            if (count_song->is_existing && !count_song->is_this_song_scheduled_for_deletion)
            {
                count++;
            }
        }
        _musicbot_store_internal__number(buffer, count);
        for (slot = 0; slot < MUSIC_BOT_MAX_FILE_COUNT; slot++)
        {
            song = &bot->music_bot_client_extension.songs[slot];
            if (!song->is_existing || song->is_this_song_scheduled_for_deletion)
            {
                continue;
            }
            _musicbot_store_internal__number(buffer, slot);
            _musicbot_store_internal__string(buffer, song->song_name);
            _musicbot_store_internal__number(buffer, song->length_seconds);
            _musicbot_store_internal__number(buffer, song->mp3_data_buffer_length);
            _musicbot_store_internal__write(buffer, song->mp3_data_buffer, song->mp3_data_buffer_length);
        }
    }
    return buffer->valid || _musicbot_store_internal__error("The music snapshot exceeds the 1 GiB limit.");
}

// Call under the clients lock. Restore also needs the channels write lock.
boole musicbot_store__prepare(char filename[64])
{
#ifndef WIN32
    int descriptor = 0; // Native file descriptor used by fdopen and close.
#endif

    uint64 index = 0;
#ifdef WIN32
    int descriptor = 0; // Native file descriptor used by fdopen and close.
#endif

    musicbot_store_buffer_t buffer = { NULL_POINTER, MUSICBOT_SNAPSHOT_MAX_BYTES, 0, TRUE };
    ubyte *encrypted = NULL_POINTER;
    uint64 encrypted_size = 0;
    ubyte random[16] = { 0 };
    FILE *file = NULL_POINTER;
    boole success = FALSE;

    filename[0] = 0;
    if (g_server_settings.music_bot_snapshot_previous[0] != 0 || g_musicbot_store_recovery_pending)
    {
        return _musicbot_store_internal__error("Restore or delete the pending snapshot before saving a new one.");
    }
    if (!_musicbot_store_internal__serialize(&buffer))
    {
        return FALSE;
    }
    if (buffer.position == sizeof(uint64))
    {
        // Keep any earlier saved playlist
        if (g_server_settings.music_bot_snapshot[0] != 0)
        {
            return _musicbot_store_internal__error("There are no music bots in RAM to save. The saved copy was kept; choose Clear from disk to remove it.");
        }
        g_musicbot_store_error[0] = 0;
        return TRUE;
    }
    buffer.size = buffer.position;
    buffer.position = 0;
    buffer.data = malloc(buffer.size);
    if (buffer.data == NULL_POINTER)
    {
        return _musicbot_store_internal__error("Not enough RAM to prepare the music snapshot.");
    }
    if (!_musicbot_store_internal__serialize(&buffer) || !crypto_storage__seal(g_server_settings.keys, g_server_settings.keys_count, buffer.data, buffer.size, &encrypted, &encrypted_size) || !util__fill_secure_random_bytes(random, sizeof(random)))
    {
        _musicbot_store_internal__error("The music snapshot could not be encrypted.");
        goto done;
    }
    strcpy(filename, "music-bots-");
    for (index = 0; index < sizeof(random); index++)
    {
        snprintf(filename + 11 + index * 2, 3, "%02x", random[index]);
    }
    strcpy(filename + 43, ".bin");
#ifdef WIN32
    descriptor = _open(filename, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (descriptor >= 0)
    {
        file = _fdopen(descriptor, "wb");
        if (file == NULL_POINTER)
        {
            _close(descriptor);
        }
    }
#else
    descriptor = open(filename, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
    if (descriptor >= 0)
    {
        file = fdopen(descriptor, "wb");
        if (file == NULL_POINTER)
        {
            close(descriptor);
        }
    }
#endif
    if (file != NULL_POINTER)
    {
        success = fwrite(encrypted, 1, encrypted_size, file) == encrypted_size && fflush(file) == 0;
#ifdef WIN32
        success = success && _commit(_fileno(file)) == 0;
#else
        success = success && fsync(fileno(file)) == 0;
#endif
        if (fclose(file) != 0)
        {
            success = FALSE;
        }
    }
    if (!success)
    {
        // An exclusive-create failure must never delete a file we did not create.
        if (descriptor >= 0 && !musicbot_store__remove_file(filename))
        {
            // Keep the name so the caller can expose the failed cleanup in settings.
            goto done;
        }
        filename[0] = 0;
        _musicbot_store_internal__error("The music snapshot could not be written; songs remain in RAM.");
    }
    else
    {
        g_musicbot_store_error[0] = 0;
    }
done:
    ciphers__clear(buffer.data, buffer.size);
    free(buffer.data);
    if (encrypted != NULL_POINTER)
    {
        ciphers__clear(encrypted, encrypted_size);
        free(encrypted);
    }
    return success;
}

// Publish prepared-snapshot state only after its settings reference was saved.
void musicbot_store__commit(void)
{
    // Preparation can precede a failed settings write. Only a committed reference
    // confirms that this saved playlist already exists in this process's RAM.
    g_musicbot_store_restored = TRUE;
}

static uint64 _musicbot_store_internal__read_number(musicbot_store_buffer_t *buffer)
{
    uint64 index = 0;
    uint64 value = 0;

    if (!buffer->valid || buffer->size - buffer->position < 8)
    {
        buffer->valid = FALSE;
        return 0;
    }
    for (index = 0; index < 8; index++)
    {
        value |= (uint64)buffer->data[buffer->position++] << (8 * index);
    }
    return value;
}

static boole _musicbot_store_internal__read_string(musicbot_store_buffer_t *buffer, char *output, uint64 capacity)
{
    uint64 size = _musicbot_store_internal__read_number(buffer);

    if (!buffer->valid || size == 0 || size >= capacity || size > buffer->size - buffer->position || memchr(buffer->data + buffer->position, 0, (uint64)size) != NULL_POINTER)
    {
        return FALSE;
    }
    memcpy(output, buffer->data + buffer->position, (uint64)size);
    output[size] = 0;
    buffer->position += (uint64)size;
    return TRUE;
}

static boole _musicbot_store_internal__restore_payload(ubyte *data, uint64 size)
{
    uint64 restore_index = 0;
    char name[USERNAME_MAX_LENGTH] = { 0 };
    uint64 channel = 0;
    uint64 paused = 0;
    uint64 songs = 0;
    client_t *bot = NULL_POINTER;
    uint64 track = 0;
    uint64 slot = 0;
    music_bot_single_song_data_t *song = NULL_POINTER;
    uint64 length = 0;
    uint64 stop_index = 0;
    uint64 join_index = 0;
    uint64 free_index = 0;
    uint64 index = 0;
    musicbot_store_buffer_t buffer = { data, size, 0, TRUE };
    uint64 count = _musicbot_store_internal__read_number(&buffer);
    uint64 allocated = 0;
    uint64 started = 0;
    client_t **bots = NULL_POINTER;
    boole success = FALSE;

    if (!buffer.valid || count > g_server_settings.max_client_count)
    {
        return FALSE;
    }
    bots = calloc(count != 0 ? (uint64)count : 1, sizeof(*bots));
    if (bots == NULL_POINTER)
    {
        return FALSE;
    }
    for (restore_index = 0; restore_index < count; restore_index++)
    {
        clib__memset(name, 0, sizeof(name));
        channel = _musicbot_store_internal__read_number(&buffer);
        if (!_musicbot_store_internal__read_string(&buffer, name, sizeof(name)))
        {
            goto done;
        }
        paused = _musicbot_store_internal__read_number(&buffer);
        songs = _musicbot_store_internal__read_number(&buffer);
        if (!buffer.valid || paused > 1 || songs > MUSIC_BOT_MAX_FILE_COUNT)
        {
            goto done;
        }
        bot = musicbot__allocate(channel, name);
        if (bot == NULL_POINTER)
        {
            goto done;
        }
        bots[allocated++] = bot;
        atomic_store(&bot->music_bot_client_extension.is_restore_pending, TRUE);
        atomic_store(&bot->music_bot_client_extension.is_playback_paused, (boole)paused);
        for (track = 0; track < songs; track++)
        {
            slot = _musicbot_store_internal__read_number(&buffer);
            if (!buffer.valid || slot >= MUSIC_BOT_MAX_FILE_COUNT)
            {
                goto done;
            }
            song = &bot->music_bot_client_extension.songs[slot];
            if (song->is_existing || !_musicbot_store_internal__read_string(&buffer, song->song_name, sizeof(song->song_name)))
            {
                goto done;
            }
            song->length_seconds = _musicbot_store_internal__read_number(&buffer);
            length = _musicbot_store_internal__read_number(&buffer);
            if (!buffer.valid || length == 0 || length > MAX_CLIENT_FILE_UPLOAD_LENGTH || length > buffer.size - buffer.position)
            {
                goto done;
            }
            song->mp3_data_buffer = (ubyte *)memorymanager__allocate(length, MEMALLOC_MUSICBOT_SONG);
            if (song->mp3_data_buffer == NULL_POINTER)
            {
                goto done;
            }
            memcpy(song->mp3_data_buffer, buffer.data + buffer.position, (uint64)length);
            buffer.position += (uint64)length;
            song->mp3_data_buffer_length = length;
            song->generation = ++bot->music_bot_client_extension.next_song_generation;
            song->is_existing = TRUE;
            bot->music_bot_client_extension.music_bot_songs_count++;
        }
    }
    if (!buffer.valid || buffer.position != buffer.size)
    {
        goto done;
    }
    for (; started < count; started++)
    {
        if (!musicbot__start(bots[started]))
        {
            goto done;
        }
    }
    success = TRUE;
done:
    if (!success)
    {
        for (stop_index = 0; stop_index < started; stop_index++)
        {
            atomic_store(&bots[stop_index]->music_bot_client_extension.is_music_bot_running, FALSE);
        }
        for (join_index = 0; join_index < started; join_index++)
        {
            pthread_join((pthread_t)bots[join_index]->music_bot_client_extension.music_bot_pthread_handle, NULL_POINTER);
        }
        for (free_index = 0; free_index < allocated; free_index++)
        {
            musicbot__free_unstarted(bots[free_index]);
        }
    }
    else
    {
        for (index = 0; index < count; index++)
        {
            atomic_store(&bots[index]->music_bot_client_extension.is_restore_pending, FALSE);
            server_msg__connection__send_client_connect_message_to_all_clients(bots[index]);
        }
    }
    free(bots);
    return success;
}

boole musicbot_store__restore(const crypto_key_t *keys, uint64 count)
{
    FILE *file = NULL_POINTER;
    boole previous_may_exist = FALSE;
    int64 length = 0;
    ubyte *file_data = NULL_POINTER;
    ubyte *data = NULL_POINTER;
    uint64 size = 0;
    uint64 file_size = 0;
    boole success = FALSE;

    if (g_musicbot_store_restored)
    {
        return _musicbot_store_internal__error("The saved songs are already in RAM and cannot be restored twice during this server run.");
    }
    if (!_musicbot_store_internal__valid_name(g_server_settings.music_bot_snapshot))
    {
        return _musicbot_store_internal__error("There is no valid saved music snapshot to restore.");
    }
    g_musicbot_store_recovery_pending = TRUE;
    file = fopen(g_server_settings.music_bot_snapshot, "rb");
    if (file == NULL_POINTER)
    {
        if (errno == ENOENT)
        {
            // A missing current file may have been consumed on an earlier startup,
            // but it may also have been lost. Never erase a surviving previous
            // snapshot on that assumption, and never replay it automatically.
            if (_musicbot_store_internal__valid_name(g_server_settings.music_bot_snapshot_previous))
            {
                previous_may_exist = _musicbot_store_internal__reference_may_exist(g_server_settings.music_bot_snapshot_previous);
                if (previous_may_exist)
                {
                    strcpy(g_server_settings.music_bot_snapshot, g_server_settings.music_bot_snapshot_previous);
                    g_server_settings.music_bot_snapshot_previous[0] = 0;
                    return _musicbot_store_internal__error("The current music snapshot is missing. A previous snapshot was kept for recovery. Restore it explicitly to recover that playlist, or choose Clear from disk.");
                }
            }
            g_server_settings.music_bot_snapshot[0] = 0;
            g_musicbot_store_error[0] = 0;
            g_musicbot_store_recovery_pending = FALSE;
            return TRUE;
        }
        return _musicbot_store_internal__error("The saved music snapshot could not be opened. Check permissions, then retry or choose Clear from disk.");
    }
    // The 1 GiB snapshot limit also fits a native 32-bit ftell result.
    if (fseek(file, 0, SEEK_END) == 0)
    {
        length = ftell(file);
        if (length > 0 && (uint64)length <= MUSICBOT_SNAPSHOT_MAX_BYTES + 256 && fseek(file, 0, SEEK_SET) == 0)
        {
            file_size = (uint64)length;
            file_data = malloc(file_size);
            success = file_data != NULL_POINTER && fread(file_data, 1, file_size, file) == file_size;
        }
    }
    fclose(file);
    if (!success)
    {
        _musicbot_store_internal__error("The snapshot could not be read or is too large. It has been kept on disk.");
        goto done;
    }
    success = FALSE;
    if (!crypto_storage__open(keys, count, file_data, file_size, &data, &size))
    {
        _musicbot_store_internal__error("Music restoration failed: wrong keys or a damaged snapshot. Supply the original keys, or choose Clear from disk.");
        goto done;
    }
    if (!_musicbot_store_internal__restore_payload(data, size))
    {
        _musicbot_store_internal__error("Music restoration failed: invalid playlist, missing/occupied channel, insufficient slots or RAM. The snapshot was kept; fix the problem and retry, or choose Clear from disk.");
        goto done;
    }
    g_musicbot_store_restored = TRUE;
    g_musicbot_store_error[0] = 0;
    g_musicbot_store_recovery_pending = FALSE;
    success = TRUE;
done:
    if (file_data != NULL_POINTER)
    {
        ciphers__clear(file_data, file_size);
        free(file_data);
    }
    return success;
}

void musicbot_store__load_reference(cJSON *settings)
{
    const char *filename = NULL_POINTER;
    const char *previous = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(settings, "music_bot_snapshot_previous"));

    if (previous != NULL_POINTER && _musicbot_store_internal__valid_name(previous))
    {
        strcpy(g_server_settings.music_bot_snapshot_previous, previous);
    }
    filename = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(settings, "music_bot_snapshot"));
    if (filename != NULL_POINTER && filename[0] != 0)
    {
        if (!_musicbot_store_internal__valid_name(filename))
        {
            _musicbot_store_internal__error("Invalid music snapshot reference in server_settings.json; no file was opened.");
            return;
        }
        strcpy(g_server_settings.music_bot_snapshot, filename);
        clib__write_lock(&g_clients_global_rwlock_guard);
        clib__write_lock(&g_channels_global_rwlock_guard);
        if (!musicbot_store__restore(g_server_settings.keys, g_server_settings.keys_count))
        {
            fprintf(stderr, "%s\n", g_musicbot_store_error);
        }
        clib__unlock(&g_channels_global_rwlock_guard);
        clib__unlock(&g_clients_global_rwlock_guard);
    }
    if (g_server_settings.music_bot_snapshot_previous[0] != 0)
    {
        // Drop only missing references
        if (!_musicbot_store_internal__reference_may_exist(g_server_settings.music_bot_snapshot_previous))
        {
            g_server_settings.music_bot_snapshot_previous[0] = 0;
        }
    }
}

boole musicbot_store__action(cJSON *message)
{
    server_settings_t recovery = { 0 };
    cJSON *supplied = NULL_POINTER;
    cJSON *wrapper = NULL_POINTER;
    boole success = FALSE;
    const crypto_key_t *keys = NULL_POINTER;
    uint64 count = 0;
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "music_bot_snapshot_action"));

    if (action != NULL_POINTER && strcmp(action, "delete") == 0)
    {
        if (!musicbot_store__remove_file(g_server_settings.music_bot_snapshot) || !musicbot_store__remove_file(g_server_settings.music_bot_snapshot_previous))
        {
            return FALSE;
        }
        g_server_settings.music_bot_snapshot[0] = 0;
        g_server_settings.music_bot_snapshot_previous[0] = 0;
        g_musicbot_store_error[0] = 0;
        g_musicbot_store_recovery_pending = FALSE;
        return TRUE;
    }
    if (action == NULL_POINTER || strcmp(action, "restore") != 0)
    {
        return _musicbot_store_internal__error("Unknown music snapshot action.");
    }
    supplied = cJSON_GetObjectItemCaseSensitive(message, "music_bot_snapshot_keys");
    wrapper = cJSON_CreateObject();
    keys = g_server_settings.keys;
    count = g_server_settings.keys_count;
    if (wrapper == NULL_POINTER)
    {
        return FALSE;
    }
    if (supplied != NULL_POINTER)
    {
        if (!cJSON_AddItemReferenceToObject(wrapper, "connection_passwords", supplied) || !settings__read_connection_encryption(wrapper, &recovery))
        {
            _musicbot_store_internal__error("Recovery keys must be valid typed passwords in their original order.");
            goto done;
        }
        keys = recovery.keys;
        count = recovery.keys_count;
    }
    clib__write_lock(&g_channels_global_rwlock_guard);
    success = musicbot_store__restore(keys, count);
    clib__unlock(&g_channels_global_rwlock_guard);
done:
    ciphers__clear(&recovery, sizeof(recovery));
    cJSON_Delete(wrapper);
    return success;
}

void musicbot_store__write_status(cJSON *message)
{
    cJSON_AddBoolToObject(message, "music_bot_snapshot_present", g_server_settings.music_bot_snapshot[0] != 0 || g_server_settings.music_bot_snapshot_previous[0] != 0);
    cJSON_AddBoolToObject(message, "music_bot_snapshot_can_restore", !g_musicbot_store_restored && g_musicbot_store_recovery_pending && g_server_settings.music_bot_snapshot[0] != 0);
    cJSON_AddStringToObject(message, "music_bot_snapshot_error", g_musicbot_store_error);
}
