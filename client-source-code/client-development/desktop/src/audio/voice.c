#include "platform/memory.h"
#include "clib.h"
// The session worker owns media authentication and encryption; audio owns devices/codecs.
#include <stdlib.h>
#include "session/session_internal.h"
#include "msg/client/client_msg.h"
#include "msg/server/server_msg.h"
#include "network/endpoint.h"
#include "platform/hotkey.h"
#include "platform/system.h"
#include "mbedtls/platform_util.h"

#define VOICE_BOTS 16

typedef struct voice_bot_t
{
    uint id;
    boole used;
    boole ready;
    uint64 requested;
    desktop_content_keys_t keys;
} voice_bot_t;

struct desktop_voice_t
{
    desktop_transport_t* transport;
    desktop_audio_t* audio;
    desktop_hotkey_t* hotkey;
    desktop_media_keys_t media;
    unsigned char auth_key[32];
    unsigned char ticket[16];
    uint64 counter;
    uint64 received_counter;
    uint64 pending_until;
    uint64 retry_after;
    uint64 hotkey_retry;
    boole received_any;
    boole have_keys;
    int64 state;
    int64 sent_state;
    boole policy_socket;
    boole policy_voice;
    boole policy_music;
    boole devices;
    voice_bot_t bots[VOICE_BOTS];
};

// declarations
static void _voice_internal__drop(desktop_voice_t* voice);
static boole _voice_internal__request(desktop_session_t* session, const char* type, int64 value);
static voice_bot_t* _voice_internal__bot(desktop_voice_t* voice, uint id);
static void _voice_internal__receive(desktop_session_t* session, const unsigned char* bytes, uint64 size);
static void _voice_internal__send(desktop_session_t* session);

/**
 * @brief copies the independent enumeration snapshot without using the session worker.
 * @param desktop_voice_t* voice -> GUI-lifetime voice engine
 * @param desktop_devices_view_t* view -> caller-owned snapshot
 * @param boole refresh -> TRUE to request enumeration
 * @return boole -> TRUE when the snapshot changed
 */
boole desktop_voice__devices(desktop_voice_t* voice, desktop_devices_view_t* view, boole refresh)
{
    return desktop_audio__devices(voice->audio, view, refresh);
}

/**
 * @brief creates independent bounded audio transport and device processing.
 * @param boole devices -> nonzero for native devices; zero for owned protocol/PCM fixtures
 * @return desktop_voice_t* -> owned state, or NULL on initialization failure
 */
desktop_voice_t* desktop_voice__create(boole devices)
{
    desktop_voice_t* voice = calloc(1, sizeof(*voice));
    if (!voice)
    {
        return NULL;
    }
    voice->transport = desktop_transport__create();
    voice->audio = desktop_audio__create(devices);
    voice->devices = devices;
    voice->sent_state = -1;
    voice->policy_socket = 1;
    voice->policy_voice = 1;
    voice->policy_music = 1;
    if (!voice->transport || !voice->audio)
    {
        desktop_voice__destroy(voice);
        return NULL;
    }
    return voice;
}

/**
 * @brief stops and joins all callbacks before releasing media state.
 * @param desktop_voice_t* voice -> owned state, or NULL
 * @return void
 */
void desktop_voice__destroy(desktop_voice_t* voice)
{
    if (voice)
    {
        desktop_voice__stop(voice);
        desktop_transport__destroy(voice->transport);
        desktop_audio__destroy(voice->audio);
        mbedtls_platform_zeroize(voice, sizeof(*voice));
        desktop_memory__erase(voice, sizeof(*voice));
        free(voice);
    }
}

/**
 * @brief closes the current media attachment and schedules a bounded reconnect delay.
 * @param desktop_voice_t* voice -> session-worker-owned media state
 * @return void
 */
static void _voice_internal__drop(desktop_voice_t* voice)
{
    if (voice->transport)
    {
        desktop_transport__disconnect(voice->transport);
    }
    voice->state = 0;
    voice->sent_state = -1;
    voice->pending_until = 0;
    voice->retry_after = desktop_system__milliseconds() + 3000;
    mbedtls_platform_zeroize(voice->auth_key, sizeof(voice->auth_key));
    mbedtls_platform_zeroize(voice->ticket, sizeof(voice->ticket));
}

/**
 * @brief ends voice for a disconnected session and forgets its keys and queued audio.
 * @param desktop_voice_t* voice -> worker-owned media state, or NULL
 * @return void
 */
void desktop_voice__stop(desktop_voice_t* voice)
{
    desktop_audio_options_t disabled = {0};
    if (!voice)
    {
        return;
    }
    _voice_internal__drop(voice);
    if (voice->audio)
    {
        desktop_audio__configure(voice->audio, &disabled);
        desktop_audio__reset(voice->audio);
    }
    desktop_hotkey__destroy(voice->hotkey);
    voice->hotkey = NULL;
    mbedtls_platform_zeroize(&voice->media, sizeof(voice->media));
    mbedtls_platform_zeroize(voice->bots, sizeof(voice->bots));
    voice->have_keys = 0;
    voice->received_any = 0;
    voice->counter = 0;
    voice->received_counter = 0;
    voice->hotkey_retry = 0;
    voice->policy_socket = 1;
    voice->policy_voice = 1;
    voice->policy_music = 1;
}

/**
 * @brief discards old-channel audio and bot keys without reusing the media frame counter.
 * @param desktop_voice_t* voice -> worker-owned media state
 * @return void
 */
void desktop_voice__channel_changed(desktop_voice_t* voice)
{
    if (voice)
    {
        desktop_audio__reset(voice->audio);
        mbedtls_platform_zeroize(voice->bots, sizeof(voice->bots));
        voice->sent_state = -1;
    }
}

/**
 * @brief sends existing audio control messages through authenticated metadata.
 * @param desktop_session_t* session -> worker-owned session
 * @param const char* type -> protocol message type
 * @param int64 value -> microphone state or bot ID; -1 omits the field
 * @return boole -> 1 when accepted by transport
 */
static boole _voice_internal__request(desktop_session_t* session, const char* type, int64 value)
{
    cJSON* message = client_msg__delivery__new_message(type);
    boole complete = message != NULL;
    if (value >= 0)
    {
        complete = complete && cJSON_AddNumberToObject(message,
            clib__strcmp(type, "microphone_usage") == 0 ? "value" : "client_id", (double)value);
    }
    return client_msg__delivery__send(session, message, complete);
}

/**
 * @brief finds bounded per-bot encryption storage by authenticated client ID.
 * @param desktop_voice_t* voice -> worker-owned media state
 * @param uint id -> server client ID
 * @return voice_bot_t* -> existing or free slot, or NULL when full
 */
static voice_bot_t* _voice_internal__bot(desktop_voice_t* voice, uint id)
{
    int64 index = 0;
    voice_bot_t* available = NULL;
    for (index = 0; index < VOICE_BOTS; index++)
    {
        if (voice->bots[index].used && voice->bots[index].id == id)
        {
            return &voice->bots[index];
        }
        if (!voice->bots[index].used && !available)
        {
            available = &voice->bots[index];
        }
    }
    if (available)
    {
        available->used = 1;
        available->id = id;
    }
    return available;
}

/**
 * @brief handles server policy, one-use audio tickets and current-channel music-bot keys.
 * @param desktop_session_t* session -> worker-owned session
 * @param const cJSON* message -> authenticated metadata message
 * @return void
 */
void desktop_voice__message(desktop_session_t* session, const cJSON* message)
{
    desktop_voice_t* voice = session->voice;
    const char* type = server_msg__read_string(message, "type");
    const cJSON* value = NULL;
    const cJSON* keys = NULL;
    desktop_endpoint_t endpoint = {0};
    char url[1024] = {0};
    voice_bot_t* bot = NULL;
    int64 id = -1;
    int64 slot = -1;
    int64 index = 0;
    if (!voice)
    {
        return;
    }
    if (clib__strcmp(type, "authentication_status") == 0 || clib__strcmp(type, "server_policy") == 0)
    {
        value = cJSON_GetObjectItemCaseSensitive(message, "is_audio_over_websocket_allowed");
        if (cJSON_IsBool(value))
        {
            voice->policy_socket = cJSON_IsTrue(value);
        }
        value = cJSON_GetObjectItemCaseSensitive(message, "is_voice_chat_active");
        if (cJSON_IsBool(value))
        {
            voice->policy_voice = cJSON_IsTrue(value);
        }
        value = cJSON_GetObjectItemCaseSensitive(message, "is_music_bot_audio_active");
        if (cJSON_IsBool(value))
        {
            voice->policy_music = cJSON_IsTrue(value);
        }
    }
    else if (clib__strcmp(type, "audio_websocket_session") == 0 && voice->state == 1
        && desktop_system__milliseconds() < voice->pending_until)
    {
        id = server_msg__read_number(message, "client_id");
        if (id != session->model.local_id || id < 0 || id > UINT32_MAX
            || !desktop_media__hex(server_msg__read_string(message, "auth_key"), voice->auth_key, 32)
            || !desktop_media__hex(server_msg__read_string(message, "ticket"), voice->ticket, 16)
            || !desktop_endpoint__parse(session->connection.address, &endpoint))
        {
            _voice_internal__drop(voice);
            return;
        }
        if (!clib__string_format(url, sizeof(url), "%s://%s%s%s:%u/audio", endpoint.secure ? "wss" : "ws",
                clib__strchr(endpoint.host, ':') ? "[" : "", endpoint.host,
                clib__strchr(endpoint.host, ':') ? "]" : "", endpoint.port)
            || !desktop_transport__connect(voice->transport, url))
        {
            _voice_internal__drop(voice);
            return;
        }
        voice->state = 2;
    }
    else if (clib__strcmp(type, "music_bot_key") == 0)
    {
        id = server_msg__read_number(message, "client_id");
        slot = desktop_session__client(session, id);
        if (slot < 0 || !session->model.clients[slot].music || session->model.clients[slot].channel != session->model.channel
            || server_msg__read_number(message, "channel_id") != session->model.channel || id > UINT32_MAX)
        {
            return;
        }
        bot = _voice_internal__bot(voice, (uint)id);
        keys = cJSON_GetObjectItemCaseSensitive(message, "music_bot_keys");
        if (!bot || !cJSON_IsArray(keys) || cJSON_GetArraySize(keys) != 4)
        {
            return;
        }
        bot->ready = 0;
        mbedtls_platform_zeroize(&bot->keys, sizeof(bot->keys));
        for (index = 0; index < 4; index++)
        {
            value = cJSON_GetArrayItem(keys, (int)index);
            if (!cJSON_IsString(value) || !desktop_media__hex(value->valuestring, bot->keys.keys[index], 32))
            {
                mbedtls_platform_zeroize(&bot->keys, sizeof(bot->keys));
                return;
            }
        }
        bot->ready = 1;
    }
}

/**
 * @brief checks media authentication, replay counter and channel membership before decoding audio.
 * @param desktop_session_t* session -> worker-owned session
 * @param const unsigned char* bytes -> complete binary frame
 * @param uint64 size -> frame byte count
 * @return void
 */
static void _voice_internal__receive(desktop_session_t* session, const unsigned char* bytes, uint64 size)
{
    desktop_voice_t* voice = session->voice;
    desktop_audio_packet_t packet = {0};
    unsigned char content[DESKTOP_MEDIA_CONTENT] = {0};
    uint64 counter = 0;
    uint64 frame_counter = 0;
    uint64 length = 0;
    int64 index = 0;
    int64 slot = -1;
    voice_bot_t* bot = NULL;
    const desktop_content_keys_t* keys = &session->keys;
    length = desktop_media__open(&voice->media, bytes, size, &packet.sender, &counter, &frame_counter, content, sizeof(content));
    if (!length || (voice->received_any && frame_counter <= voice->received_counter))
    {
        return;
    }
    voice->received_counter = frame_counter;
    voice->received_any = 1;
    slot = desktop_session__client(session, packet.sender);
    if (slot < 0 || packet.sender == session->model.local_id || session->model.clients[slot].muted
        || session->model.clients[slot].idle || session->model.clients[slot].channel != session->model.channel)
    {
        return;
    }
    if (session->model.clients[slot].music)
    {
        bot = _voice_internal__bot(voice, packet.sender);
        if (!bot)
        {
            return;
        }
        if (!bot->ready)
        {
            if (desktop_system__milliseconds() - bot->requested >= 1000)
            {
                bot->requested = desktop_system__milliseconds();
                _voice_internal__request(session, "request_music_bot_key", packet.sender);
            }
            return;
        }
        keys = &bot->keys;
    }
    else if (!session->model.keys_ready)
    {
        return;
    }
    for (index = 4; index > 0; index--)
    {
        desktop_media__layer(keys->keys[index - 1], keys->ivs[index - 1], packet.sender, counter, content, length);
    }
    packet.sequence = (ushort)(content[0] | (content[1] << 8));
    packet.size = length - 2;
    clib__memcpy(packet.data, content + 2, packet.size);
    packet.volume_offset = session->model.clients[slot].volume_offset;
    if (desktop_audio__receive(voice->audio, &packet))
    {
        session->model.audio_packets_received++;
    }
    clib__null_memory(content, sizeof(content));
    clib__null_memory(&packet, sizeof(packet));
}

/**
 * @brief sends a bounded batch of fresh microphone packets with unique media counters.
 * @param desktop_session_t* session -> worker-owned session with acknowledged talk permission
 * @return void
 */
static void _voice_internal__send(desktop_session_t* session)
{
    desktop_voice_t* voice = session->voice;
    desktop_audio_packet_t packet = {0};
    unsigned char content[DESKTOP_MEDIA_CONTENT] = {0};
    unsigned char frame[DESKTOP_MEDIA_FRAME] = {0};
    int64 batch = 0;
    int64 index = 0;
    uint64 size = 0;
    for (batch = 0; batch < 4 && desktop_audio__next_packet(voice->audio, &packet); batch++)
    {
        if (voice->counter == UINT64_MAX)
        {
            desktop_session__detail(session, "Media counter exhausted. Reconnect before transmitting again.", 1);
            return;
        }
        voice->counter++;
        content[0] = (unsigned char)packet.sequence;
        content[1] = (unsigned char)(packet.sequence >> 8);
        clib__memcpy(content + 2, packet.data, packet.size);
        for (index = 0; index < 4; index++)
        {
            desktop_media__layer(session->keys.keys[index], session->keys.ivs[index], (uint)session->model.local_id,
                voice->counter, content, packet.size + 2);
        }
        size = desktop_media__seal(&voice->media, (uint)session->model.local_id, voice->counter,
            content, packet.size + 2, frame, sizeof(frame));
        if (!size || !desktop_transport__send_media(voice->transport, frame, size, packet.created + DESKTOP_AUDIO_PACKET_AGE))
        {
            // Live audio is dropped under backpressure instead of growing a delay queue.
            break;
        }
        session->model.audio_packets_sent++;
    }
    clib__null_memory(content, sizeof(content));
    clib__null_memory(&packet, sizeof(packet));
}

/**
 * @brief advances audio attachment and push-to-talk without doing device work on the session worker.
 * @param desktop_session_t* session -> authenticated worker-owned session
 * @return void
 */
void desktop_voice__tick(desktop_session_t* session)
{
    desktop_voice_t* voice = session->voice;
    desktop_audio_options_t options = {0};
    desktop_audio_status_t audio = {0};
    desktop_transport_status_t status = {0};
    desktop_message_t message = {0};
    unsigned char proof[53] = {0};
    uint64 now = desktop_system__milliseconds();
    int64 batch = 0;
    int64 key = DESKTOP_HOTKEY_DEFAULT;
    int64 requested = 3;
    int64 slot = desktop_session__client(session, session->model.local_id);
    if (!voice)
    {
        return;
    }
    desktop_mutex__lock(session->mutex);
    options = session->audio_options;
    key = session->push_to_talk_key;
    desktop_mutex__unlock(session->mutex);
    session->model.audio_ready = 0;
    session->model.microphone_ready = 0;
    session->model.talking = 0;
    session->model.voice_detected = FALSE;
    session->model.input_level = -90;
    if (!options.enabled || !voice->policy_socket || (!voice->policy_voice && !voice->policy_music)
        || !session->model.connected || slot < 0 || session->model.clients[slot].idle)
    {
        if (voice->state)
        {
            _voice_internal__drop(voice);
            desktop_audio__reset(voice->audio);
        }
        options.enabled = 0;
        desktop_audio__configure(voice->audio, &options);
        clib__string_copy(session->model.audio_detail, sizeof(session->model.audio_detail),
            !voice->policy_socket ? "This server does not allow audio over TCP WebSocket." : "Audio disabled");
        return;
    }
    if (!voice->have_keys)
    {
        if (!desktop_crypto__media_keys(session->crypto, &voice->media)
            || !desktop_crypto__random(session->crypto, (unsigned char*)&voice->counter, sizeof(voice->counter)))
        {
            return;
        }
        voice->counter &= UINT64_MAX >> 1;
        voice->have_keys = 1;
    }
    if (!voice->state && now >= voice->retry_after)
    {
        voice->state = 1;
        voice->pending_until = now + 10000;
        _voice_internal__request(session, "request_audio_websocket", -1);
    }
    desktop_transport__status(voice->transport, &status);
    if (voice->state >= 2 && (status.state == DESKTOP_TRANSPORT_FAILED || status.state == DESKTOP_TRANSPORT_DISCONNECTED))
    {
        _voice_internal__drop(voice);
    }
    if (voice->state == 2 && status.state == DESKTOP_TRANSPORT_CONNECTED)
    {
        if (desktop_media__proof(voice->auth_key, voice->ticket, (uint)session->model.local_id, proof)
            && desktop_transport__send_binary(voice->transport, proof, sizeof(proof)))
        {
            voice->state = 3;
        }
    }
    for (batch = 0; batch < 32 && desktop_transport__receive(voice->transport, &message); batch++)
    {
        if (!message.binary)
        {
            _voice_internal__drop(voice);
        }
        else if (voice->state == 3)
        {
            if (desktop_media__acknowledgement(voice->auth_key, voice->ticket, (const unsigned char*)message.data, message.size))
            {
                voice->state = 4;
                voice->pending_until = 0;
                voice->sent_state = -1;
                mbedtls_platform_zeroize(voice->auth_key, sizeof(voice->auth_key));
            }
            else
            {
                _voice_internal__drop(voice);
            }
        }
        else if (voice->state == 4)
        {
            if (message.size)
            {
                _voice_internal__receive(session, (const unsigned char*)message.data, message.size);
            }
            else
            {
                desktop_transport__send_binary(voice->transport, NULL, 0);
            }
        }
        desktop_message__release(&message);
    }
    if (voice->state && voice->state < 4 && now >= voice->pending_until)
    {
        _voice_internal__drop(voice);
    }
    options.enabled = voice->state == 4;
    options.microphone = options.enabled && options.microphone && voice->policy_voice;
    if (options.microphone && !options.voice_activation && !voice->hotkey && voice->devices && now >= voice->hotkey_retry)
    {
        voice->hotkey = desktop_hotkey__create();
        voice->hotkey_retry = now + 3000;
    }
    desktop_audio__status(voice->audio, &audio);
    if (options.microphone)
    {
        requested = (options.voice_activation ? audio.voice_active : (options.transmit || desktop_hotkey__down(voice->hotkey, key))) && audio.capture_ready
            && session->model.keys_ready ? 1 : 2;
    }
    if (voice->state == 4 && voice->sent_state != requested)
    {
        if (_voice_internal__request(session, "microphone_usage", requested))
        {
            voice->sent_state = requested;
        }
    }
    options.transmit = requested == 1 && session->model.clients[slot].audio == 1 && session->model.keys_ready;
    desktop_audio__configure(voice->audio, &options);
    if (options.transmit)
    {
        _voice_internal__send(session);
    }
    else
    {
        desktop_transport__discard_media(voice->transport);
    }
    session->model.audio_ready = voice->state == 4 && audio.playback_ready;
    session->model.microphone_ready = options.microphone && audio.capture_ready;
    session->model.talking = options.transmit && audio.transmitting;
    session->model.input_level = options.microphone && audio.capture_ready ? audio.input_level : -90;
    session->model.voice_detected = options.microphone && audio.voice_active;
    if (voice->state != 4)
    {
        clib__string_copy(session->model.audio_detail, sizeof(session->model.audio_detail), "Connecting audio...");
    }
    else if (!audio.playback_ready || (options.microphone && !audio.capture_ready))
    {
        clib__string_copy(session->model.audio_detail, sizeof(session->model.audio_detail), audio.detail[0] ? audio.detail : "Opening audio devices...");
    }
    else if (options.microphone && !options.voice_activation && voice->devices && !voice->hotkey)
    {
        clib__string_copy(session->model.audio_detail, sizeof(session->model.audio_detail), "Push-to-talk key access is unavailable.");
    }
    else if (options.microphone && options.voice_activation)
    {
        clib__string_copy(session->model.audio_detail, sizeof(session->model.audio_detail),
            session->model.talking ? "Talking - voice activation" : "Voice activation - listening for speech");
    }
    else
    {
        clib__string_format(session->model.audio_detail, sizeof(session->model.audio_detail),
            session->model.talking ? "Talking" : options.microphone ? "Hold %s to talk" : "Listening - microphone disabled", desktop_hotkey__name(key));
    }
    mbedtls_platform_zeroize(proof, sizeof(proof));
}
