# JSON message bodies

This is the body reference for every current client request and server response in the chat JSON protocol. Each type has its own complete JSON example and field table. Server announcements use the same response bodies documented here. Types with the same spelling in both directions have separate entries.

The reference covers 82 client request types, 91 server response types (including the dormant `channel_chat_picture` serializer), the encrypted transport wrapper, and the JSON carried inside encrypted content. Binary media frames, local worker messages and saved configuration files are separate formats.

**Direct-message content, including edits sent by the updated client, is end-to-end encrypted and is not visible to the server.** The server can read the outer routing fields and the encrypted `value` string, but not the plaintext inside it. Decrypted content examples in this document describe client-side data. The insecure legacy edit command is identified separately under [Server visibility of direct-message bodies](#server-visibility-of-direct-message-bodies).

## Reading the examples

Application-body examples show JSON after the outer transport encryption is removed. This does not decrypt the content inside a direct message's `value`: that string still contains end-to-end encrypted content. Sections explicitly labeled decrypted or formatted content show client-side plaintext instead. Fields listed as `message.x` are inside the outer `message` object. `[].x` identifies a field on each array element. A child marked required is required when its containing object or array element is present.

A field marked optional or conditional may be absent; omission does not imply JSON `null`. Only fields explicitly listed as nullable use `null`. An empty string, empty array and empty object remain distinct values. The field tables state conditional rules even when an example includes optional data.

IDs, counts, sizes and durations are JSON numbers unless a field explicitly says string. Use whole numbers for integer-valued fields. Many server IDs and counters are emitted through JSON doubles; values beyond JavaScript's exact integer range must not be assumed to retain arbitrary 64-bit precision. Booleans are JSON `true`/`false`, not quoted text or 0/1. Field names and type strings are case-sensitive.

Strings such as `<RSA public key>`, `<base64 ciphertext>` and `<SDP answer lines>` stand for actual encoded data. They illustrate placement and JSON types, not usable keys, images or media descriptions. Hex keys and timestamps are also illustrative. Every fenced JSON block is valid JSON; JSON embedded inside a string remains escaped.

## Contents

Choose the feature area below, then search within that section for the exact request or response type. Client requests and server responses are documented separately even when their names are similar.

- [Application and transport wrappers](#application-and-transport-wrappers)
- [Server visibility of direct-message bodies](#server-visibility-of-direct-message-bodies)
- [Connection and client state](#connection-and-client-state)
- [Channels and idle state](#channels-and-idle-state)
- [Chat and URL preview bodies](#chat-and-url-preview-bodies)
- [Picture and file bodies](#picture-and-file-bodies)
- [Profiles and stored identities](#profiles-and-stored-identities)
- [Administration and server groups](#administration-and-server-groups)
- [Channel moderation bodies](#channel-moderation-bodies)
- [Server settings and public information](#server-settings-and-public-information)
- [Server emoji bodies](#server-emoji-bodies)
- [Audio and media connection bodies](#audio-and-media-connection-bodies)
- [Video stream bodies](#video-stream-bodies)
- [Music and music-bot bodies](#music-and-music-bot-bodies)
- [JSON inside content fields](#json-inside-content-fields)
- [Compatibility body names](#compatibility-body-names)
- [Implementation sources](#implementation-sources)

## Application and transport wrappers

The chat WebSocket uses application close code `4001` when a connected client
is kicked from the server and `4002` when it is banned. These are transport close
frames, not JSON messages. Browser and desktop clients use them to display the
reason and play `you_were_kicked_from_the_server` or
`you_were_banned_from_the_server` once, instead of the generic connection-lost
sound. The browser waits for an explicit connect request after either action.
Other close codes retain the normal disconnect behavior. A ban rejected before
login does not trigger the sound for removal from an authenticated session.

### Application wrapper

```json
{
    "message":
    {
        "type": "client_connection_check"
    }
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message` | object | Required | Application body object. |
| `message.type` | string | Required | Nonempty, case-sensitive message type. Every request/response example below supplies its exact literal. |

The body fields depend on `message.type`. There is no universal `status`, `result`, `error`, `success` or `request_id` member. A body described as having no additional fields consists of `message.type` alone.

### Encrypted transport wrapper

```json
{
    "iv": "<base64 IV>",
    "data": "<base64 encrypted application JSON>",
    "tag": "<base64 HMAC tag>"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `iv` | string | Required | Base64-encoded 16-byte IV. |
| `data` | string | Required | Base64 ciphertext containing the application JSON. |
| `tag` | string | Conditional: authenticated session envelope | Base64-encoded 32-byte authentication tag. Required when the session authentication key is established; absent in the pre-session form. |

The pre-session form has `iv` and `data` without a session `tag`. The `rsa_key_too_weak` body is the explicit plaintext application-wrapper exception.

### Server visibility of direct-message bodies

For `direct_chat_message` and `offline_chat_message`, the outer body is readable by the server, while the message content inside `value` remains encrypted. The server does not possess the recipient's private key or the plaintext per-message content keys. Removing the transport wrapper therefore does not reveal the direct-message text.

| Body member or content | Visible to the server? |
| --- | --- |
| `type`, recipient ID or identity hash, local/server message IDs, sender identity/name, and offline queue metadata | Yes, where present in the outer request or response. |
| The serialized `value` envelope, including encrypted `message_keys`, IV and ciphertext | Yes, as encoded data. Parsing the envelope does not reveal plaintext content or usable content keys. |
| Decrypted direct text, replacement text for edits, formatting, quoted text and reaction data inside `value` | No. These are client-side plaintext covered by content encryption. |
| The inner `message_seen` or `channel_keys_from_maintainer` control body inside a direct-message `value` | No. Its inner type and fields remain encrypted. |
| Direct attachment routing fields, message IDs and encrypted upload lengths | Yes. Direct picture/file content and the decrypted filename, MIME type and original file size remain inside encrypted content. |

**Encrypted edits:** the updated client places the [edit control body](#edit-control-body) inside encrypted `direct_chat_message` or `channel_chat_message` content. The replacement text and target reference are encrypted together.

**Insecure legacy commands:** the server still accepts `edit_chat_message_request` and can emit the outer `chat_message_edit` response for older clients. Their `new_message_value` is server-readable. The updated client never sends that request and ignores that response. All participants need the updated client for encrypted editing; an older client can still disclose an edit through the legacy command.

## Connection and client state

| Direction | Type |
| --- | --- |
| Client request | [`public_key_info`](#client-request-public_key_info) |
| Client request | [`public_key_challenge_response`](#client-request-public_key_challenge_response) |
| Client request | [`client_connection_check`](#client-request-client_connection_check) |
| Server response | [`public_key_challenge`](#server-response-public_key_challenge) |
| Server response | [`rsa_key_too_weak`](#server-response-rsa_key_too_weak) |
| Server response | [`authentication_status`](#server-response-authentication_status) |
| Server response | [`fast_reconnect_ok`](#server-response-fast_reconnect_ok) |
| Server response | [`connection_check_response`](#server-response-connection_check_response) |
| Server response | [`client_list`](#server-response-client_list) |
| Server response | [`client_connect`](#server-response-client_connect) |
| Server response | [`client_disconnect`](#server-response-client_disconnect) |

### Client request: `public_key_info`

**Body**

```json
{
    "message":
    {
        "type": "public_key_info",
        "value": "<base64 RSA modulus>",
        "verification_string": "welcome",
        "dh_public_mix": "<decimal DH public value>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"public_key_info"`. |
| `message.value` | string | Required | Base64 RSA modulus representation; length 344–1368 characters and divisible by four. This is the public key, not a PEM object. |
| `message.verification_string` | string | Required | Literal string `"welcome"`. |
| `message.dh_public_mix` | string | Required | Decimal-digit string containing the client Diffie–Hellman public value. It must be nonempty and no longer than the decimal modulus string. |

### Client request: `public_key_challenge_response`

**Body**

```json
{
    "message":
    {
        "type": "public_key_challenge_response",
        "value": "<challenge response>",
        "chosen_username": "Alice",
        "fast_reconnect": false
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"public_key_challenge_response"`. |
| `message.value` | string | Required | Decrypted RSA challenge value as a string. |
| `message.chosen_username` | string | Optional | Requested display name, 1–50 UTF-8 characters. An absent or unusable name leaves automatic naming available. |
| `message.fast_reconnect` | boolean | Optional; absent means false | Whether this body requests reuse of an existing session. |

### Client request: `client_connection_check`

**Body**

```json
{
    "message":
    {
        "type": "client_connection_check"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_connection_check"`. |

No additional body fields.

### Server response: `public_key_challenge`

**Body**

```json
{
    "message":
    {
        "type": "public_key_challenge",
        "value": "<RSA-encrypted challenge>",
        "dh_public_mix": "<decimal DH public value>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"public_key_challenge"`. |
| `message.value` | string | Required | RSA-encrypted challenge string. |
| `message.dh_public_mix` | string | Required | Server Diffie–Hellman public value as a decimal string. |

### Server response: `rsa_key_too_weak`

This response uses the plaintext `message` wrapper rather than the encrypted transport envelope.

**Body**

```json
{
    "message":
    {
        "type": "rsa_key_too_weak",
        "minimum_rsa_key_bits": 4096
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"rsa_key_too_weak"`. |
| `message.minimum_rsa_key_bits` | number | Required | Minimum accepted RSA key size in bits. |

### Server response: `authentication_status`

**Body**

```json
{
    "message":
    {
        "type": "authentication_status",
        "value": "success",
        "is_audio_over_websocket_allowed": true,
        "is_audio_over_webrtc_allowed": true,
        "is_voice_chat_active": true,
        "is_music_bot_audio_active": true,
        "is_video_streaming_allowed": true,
        "is_idle_mode_allowed": true,
        "is_fast_reconnect_allowed": true,
        "is_permission_system_enabled": true,
        "is_stored_client_list_allowed": true,
        "is_connection_check_enforced": true,
        "is_channel_key_reuse_enabled": true,
        "is_channel_key_reuse_on_join_allowed": true,
        "is_channel_key_reuse_on_leave_allowed": true,
        "is_music_bot_marquee_visible_to_everyone": true,
        "is_avatar_upload_allowed": true,
        "is_typing_indicator_allowed": true,
        "is_client_rename_allowed": true,
        "is_link_preview_allowed": true,
        "is_file_upload_allowed": true,
        "is_chat_picture_allowed": true,
        "is_blocking_direct_chat_pictures_active": true,
        "is_blocking_direct_chat_files_active": true,
        "is_blocking_channel_chat_pictures_active": true,
        "is_blocking_channel_chat_files_active": true,
        "channel_key_reuse_join_min_clients": 30,
        "channel_key_reuse_leave_min_clients": 30,
        "avatar_max_size": 92160,
        "icon_max_size": 30000,
        "file_upload_max_size": 10485760,
        "chat_picture_max_size": 5242880,
        "stun_port": 3478
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"authentication_status"`. |
| `message.value` | string | Required | Literal `"success"`. There is no current `"failure"` body for this type. |
| `message.is_audio_over_websocket_allowed` | boolean | Required | Whether audio WebSocket connections are enabled. |
| `message.is_audio_over_webrtc_allowed` | boolean | Required | Whether WebRTC audio connections are enabled. |
| `message.is_voice_chat_active` | boolean | Required | Whether voice chat is enabled server-wide. |
| `message.is_music_bot_audio_active` | boolean | Required | Whether music-bot audio is enabled server-wide. |
| `message.is_video_streaming_allowed` | boolean | Required | Whether video streaming is enabled server-wide. |
| `message.is_idle_mode_allowed` | boolean | Required | Whether idle mode is enabled. |
| `message.is_fast_reconnect_allowed` | boolean | Required | Whether fast reconnection is enabled. |
| `message.is_permission_system_enabled` | boolean | Required | Whether the permission system is enabled. |
| `message.is_stored_client_list_allowed` | boolean | Required | Whether stored-user visibility is enabled together with identity storage. |
| `message.is_connection_check_enforced` | boolean | Required | Whether application connection checks are enforced. |
| `message.is_channel_key_reuse_enabled` | boolean | Required | Whether channel-key reuse is enabled. |
| `message.is_channel_key_reuse_on_join_allowed` | boolean | Required | Whether channel keys may be reused when a client joins. |
| `message.is_channel_key_reuse_on_leave_allowed` | boolean | Required | Whether channel keys may be reused when a client leaves. |
| `message.is_music_bot_marquee_visible_to_everyone` | boolean | Required | Whether a music bot's song label is visible outside its channel. |
| `message.is_avatar_upload_allowed` | boolean | Required | Whether avatar uploads are enabled. |
| `message.is_typing_indicator_allowed` | boolean | Required | Whether typing indicators are enabled. |
| `message.is_client_rename_allowed` | boolean | Required | Whether client renaming is enabled. |
| `message.is_link_preview_allowed` | boolean | Required | Whether URL previews are enabled. |
| `message.is_file_upload_allowed` | boolean | Required | Whether chat file uploads are enabled. |
| `message.is_chat_picture_allowed` | boolean | Required | Whether chat picture uploads are enabled. |
| `message.is_blocking_direct_chat_pictures_active` | boolean | Required | Whether recipient blocking preferences are active for direct-chat pictures. |
| `message.is_blocking_direct_chat_files_active` | boolean | Required | Whether recipient blocking preferences are active for direct-chat files. |
| `message.is_blocking_channel_chat_pictures_active` | boolean | Required | Whether recipient blocking preferences are active for channel-chat pictures. |
| `message.is_blocking_channel_chat_files_active` | boolean | Required | Whether recipient blocking preferences are active for channel-chat files. |
| `message.channel_key_reuse_join_min_clients` | number | Required | Minimum channel client count for key reuse on join. |
| `message.channel_key_reuse_leave_min_clients` | number | Required | Minimum channel client count for key reuse on leave. |
| `message.avatar_max_size` | number | Required | Avatar size limit in bytes. |
| `message.icon_max_size` | number | Required | Icon size limit in bytes. |
| `message.file_upload_max_size` | number | Required | Chat file size limit in bytes. |
| `message.chat_picture_max_size` | number | Required | Chat picture size limit in bytes. |
| `message.stun_port` | number | Required | STUN port number: `0` in WireGuard mode, otherwise `3478`. |

### Server response: `fast_reconnect_ok`

**Body**

```json
{
    "message":
    {
        "type": "fast_reconnect_ok",
        "client_id": 7,
        "channel_id": 3
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"fast_reconnect_ok"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.channel_id` | number | Required | Numeric channel ID. |

### Server response: `connection_check_response`

**Body**

```json
{
    "message":
    {
        "type": "connection_check_response"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"connection_check_response"`. |

No additional body fields.

### Server response: `client_list`

The example includes the conditional music-key fields to show their shape; those fields belong only to eligible music-bot entries.

**Body**

```json
{
    "message":
    {
        "type": "client_list",
        "local_username": "Alice",
        "clients":
        [
            {
                "username": "Radio",
                "public_key": "<RSA public key>",
                "identity_hash": "<base64 identity hash>",
                "channel_id": 3,
                "channel_group_id": 0,
                "client_id": 7,
                "is_music_bot": true,
                "audio_state": 2,
                "country_iso_code": "SK",
                "group_ids":
                [2],
                "is_idle": false,
                "is_streaming_song": false,
                "is_streaming_video": false,
                "song_name": "",
                "music_bot_encryption": 1,
                "music_bot_keys":
                [
                    "0101010101010101010101010101010101010101010101010101010101010101",
                    "0202020202020202020202020202020202020202020202020202020202020202",
                    "0303030303030303030303030303030303030303030303030303030303030303",
                    "0404040404040404040404040404040404040404040404040404040404040404"
                ]
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_list"`. |
| `message.local_username` | string | Required | The receiving client's current display name. |
| `message.clients` | object[] | Required | Client entries. The array can be empty; each entry uses the fields below. |
| `message.clients[].username` | string | Required | Client display name. |
| `message.clients[].public_key` | string | Required | RSA public-key representation as a string. |
| `message.clients[].identity_hash` | string | Required | Base64 identity hash derived from the public key. |
| `message.clients[].channel_id` | number | Required | Visible channel ID; `-1` can represent concealed membership. |
| `message.clients[].channel_group_id` | number | Required | Channel role ID: `0` ordinary member, `1` Channel Admin, `2`–`127` a custom role. Concealed or idle membership uses `0`. |
| `message.clients[].client_id` | number | Required | Numeric client slot ID. |
| `message.clients[].is_music_bot` | boolean | Required | Whether this entry represents a music bot. |
| `message.clients[].audio_state` | number | Required | `1` transmitting; `2` push-to-talk enabled; `3` microphone disabled, reception enabled; `4` all audio disabled. |
| `message.clients[].country_iso_code` | string | Required | Two-letter country code; `""` when unknown or hidden. |
| `message.clients[].group_ids` | number[] | Required | Server-group IDs; `[]` means no listed memberships. |
| `message.clients[].is_idle` | boolean | Required | Whether the client is in idle mode. |
| `message.clients[].is_streaming_song` | boolean | Required | Whether the client is streaming a song. |
| `message.clients[].is_streaming_video` | boolean | Required | Whether the client is streaming video. |
| `message.clients[].song_name` | string | Required | Song display name; `""` when no song name is set. |
| `message.clients[].music_bot_encryption` | number | Conditional: bot has audio keys | Encryption format version. The current value is exactly `1`. |
| `message.clients[].music_bot_keys` | string[] | Conditional: bot has keys and recipient is active in its channel | Exactly four 64-character lowercase hexadecimal strings, each encoding a 32-byte audio key. Present only when the bot has keys and the recipient is non-idle in the bot's channel. |

### Server response: `client_connect`

This body has no `is_idle`, `is_streaming_song`, `is_streaming_video` or `song_name` fields. The example includes conditional bot-key fields.

**Body**

```json
{
    "message":
    {
        "type": "client_connect",
        "username": "Radio",
        "public_key": "<RSA public key>",
        "identity_hash": "<base64 identity hash>",
        "channel_id": 3,
        "channel_group_id": 0,
        "client_id": 7,
        "is_music_bot": true,
        "audio_state": 2,
        "country_iso_code": "SK",
        "group_ids":
        [2],
        "music_bot_encryption": 1,
        "music_bot_keys":
        [
            "0101010101010101010101010101010101010101010101010101010101010101",
            "0202020202020202020202020202020202020202020202020202020202020202",
            "0303030303030303030303030303030303030303030303030303030303030303",
            "0404040404040404040404040404040404040404040404040404040404040404"
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_connect"`. |
| `message.username` | string | Required | Client display name. |
| `message.public_key` | string | Required | RSA public-key representation as a string. |
| `message.identity_hash` | string | Required | Base64 identity hash derived from the public key. |
| `message.channel_id` | number | Required | Visible channel ID; `-1` can represent concealed membership. |
| `message.channel_group_id` | number | Required | Channel role ID: `0` ordinary member, `1` Channel Admin, `2`–`127` a custom role. Concealed or idle membership uses `0`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.is_music_bot` | boolean | Required | Whether this entry represents a music bot. |
| `message.audio_state` | number | Required | `1` transmitting; `2` push-to-talk enabled; `3` microphone disabled, reception enabled; `4` all audio disabled. |
| `message.country_iso_code` | string | Required | Two-letter country code; `""` when unknown or hidden. |
| `message.group_ids` | number[] | Required | Server-group IDs; `[]` means no listed memberships. |
| `message.music_bot_encryption` | number | Conditional: bot has audio keys | Encryption format version. The current value is exactly `1`. |
| `message.music_bot_keys` | string[] | Conditional: bot has keys and recipient is active in its channel | Exactly four 64-character lowercase hexadecimal strings, each encoding a 32-byte audio key. Present only when the bot has keys and the recipient is non-idle in the bot's channel. |

### Server response: `client_disconnect`

**Body**

```json
{
    "message":
    {
        "type": "client_disconnect",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_disconnect"`. |
| `message.client_id` | number | Required | Client slot ID of the disconnected client. |
| `message.reason` | string | Optional | `"ban"` when a moderator banned the client; absent for other disconnects. |

## Channels and idle state

| Direction | Type |
| --- | --- |
| Client request | [`create_channel_request`](#client-request-create_channel_request) |
| Client request | [`edit_channel_request`](#client-request-edit_channel_request) |
| Client request | [`join_channel_request`](#client-request-join_channel_request) |
| Client request | [`delete_channel_request`](#client-request-delete_channel_request) |
| Client request | [`set_channel_icon`](#client-request-set_channel_icon) |
| Client request | [`reset_channel_maintainer`](#client-request-reset_channel_maintainer) |
| Client request | [`go_to_idle_mode_request`](#client-request-go_to_idle_mode_request) |
| Client request | [`come_back_from_idle_mode_request`](#client-request-come_back_from_idle_mode_request) |
| Client request | [`call_idle_client_request`](#client-request-call_idle_client_request) |
| Server response | [`channel_list`](#server-response-channel_list) |
| Server response | [`channel_create`](#server-response-channel_create) |
| Server response | [`channel_edit`](#server-response-channel_edit) |
| Server response | [`channel_join`](#server-response-channel_join) |
| Server response | [`channel_maintainer_id`](#server-response-channel_maintainer_id) |
| Server response | [`channel_delete`](#server-response-channel_delete) |
| Server response | [`channel_icon_changed`](#server-response-channel_icon_changed) |
| Server response | [`channel_full`](#server-response-channel_full) |
| Server response | [`client_going_to_idle_mode`](#server-response-client_going_to_idle_mode) |
| Server response | [`client_coming_back_from_idle_mode`](#server-response-client_coming_back_from_idle_mode) |
| Server response | [`call`](#server-response-call) |

### Client request: `create_channel_request`

`is_temp_channel` is not a creation-body override.

**Body**

```json
{
    "message":
    {
        "type": "create_channel_request",
        "parent_channel_id": 0,
        "channel_name": "General",
        "channel_description": "General discussion",
        "channel_password": "",
        "is_audio_enabled": true,
        "is_video_stream_enabled": true,
        "is_client_limit_active": true,
        "max_client_count": 20,
        "required_talk_power": 25,
        "required_message_power": 25,
        "required_picture_power": 25,
        "required_stream_power": 25
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"create_channel_request"`. |
| `message.parent_channel_id` | number | Required | Parent channel ID. |
| `message.channel_name` | string | Required | Nonempty channel display name, at most 50 UTF-8 characters. |
| `message.channel_description` | string | Required | Channel description; an empty string is allowed. Shorter than `CHANNEL_DESCRIPTION_MAX_LENGTH` (1000). |
| `message.channel_password` | string | Required | Password string, at most 30 UTF-8 characters; `""` removes or leaves the channel without a password. |
| `message.is_audio_enabled` | boolean | Required | Whether this channel permits audio. |
| `message.is_video_stream_enabled` | boolean | Optional; defaults to true | Whether this channel permits video streams. |
| `message.is_client_limit_active` | boolean | Optional; defaults to false, including on edit | Whether the channel client limit is enabled. |
| `message.max_client_count` | number | Optional; relevant when the limit is active | Maximum channel occupancy, within the server client capacity. |
| `message.required_talk_power` | number | Optional; defaults to 0 | Minimum talk power. Whole number from `0` through `1000`. |
| `message.required_message_power` | number | Optional; defaults to 0 | Minimum channel-text power. Whole number from `0` through `1000`. |
| `message.required_picture_power` | number | Optional; defaults to 0 | Minimum channel-picture power. Whole number from `0` through `1000`. |
| `message.required_stream_power` | number | Optional; defaults to 0 | Minimum video-stream power. Whole number from `0` through `1000`. |

### Client request: `edit_channel_request`

**Body**

```json
{
    "message":
    {
        "type": "edit_channel_request",
        "channel_id": 3,
        "channel_name": "General",
        "channel_description": "General discussion",
        "is_audio_enabled": true,
        "channel_password": "",
        "is_temp_channel": false,
        "is_video_stream_enabled": true,
        "is_client_limit_active": true,
        "max_client_count": 20,
        "required_talk_power": 25,
        "required_message_power": 25,
        "required_picture_power": 25,
        "required_stream_power": 25
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"edit_channel_request"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.channel_name` | string | Required | Nonempty channel display name, at most 50 UTF-8 characters. |
| `message.channel_description` | string | Required | Channel description; an empty string is allowed. Shorter than `CHANNEL_DESCRIPTION_MAX_LENGTH` (1000). |
| `message.is_audio_enabled` | boolean | Required | Whether this channel permits audio. |
| `message.channel_password` | string | Optional | Password string, at most 30 UTF-8 characters. Omission retains the current password; `""` removes it; explicit `null` is invalid. |
| `message.is_temp_channel` | boolean | Optional; absent keeps current type | Requested channel type: true for temporary, false for permanent. |
| `message.is_video_stream_enabled` | boolean | Optional; absent keeps current value | Whether this channel permits video streams. |
| `message.is_client_limit_active` | boolean | Optional; defaults to false, including on edit | Whether the channel client limit is enabled. |
| `message.max_client_count` | number | Optional; relevant when the limit is active | Maximum channel occupancy, within the server client capacity. |
| `message.required_talk_power` | number | Optional; absent keeps current value | Minimum talk power. Whole number from `0` through `1000`. |
| `message.required_message_power` | number | Optional; absent keeps current value | Minimum channel-text power. Whole number from `0` through `1000`. |
| `message.required_picture_power` | number | Optional; absent keeps current value | Minimum channel-picture power. Whole number from `0` through `1000`. |
| `message.required_stream_power` | number | Optional; absent keeps current value | Minimum video-stream power. Whole number from `0` through `1000`. |

### Client request: `join_channel_request`

**Body**

```json
{
    "message":
    {
        "type": "join_channel_request",
        "channel_id": 3,
        "channel_password": ""
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"join_channel_request"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.channel_password` | string | Required | Password string, at most 30 UTF-8 characters; `""` removes or leaves the channel without a password. |

### Client request: `delete_channel_request`

**Body**

```json
{
    "message":
    {
        "type": "delete_channel_request",
        "channel_id": 3
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"delete_channel_request"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |

### Client request: `set_channel_icon`

**Body**

```json
{
    "message":
    {
        "type": "set_channel_icon",
        "channel_id": 3,
        "icon_id": 4
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"set_channel_icon"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.icon_id` | number or null | Optional | Existing server icon ID. An omitted field or explicit `null` removes the icon. |

**Clear the icon**

```json
{
    "message":
    {
        "type": "set_channel_icon",
        "channel_id": 3,
        "icon_id": null
    }
}
```

### Client request: `reset_channel_maintainer`

**Body**

```json
{
    "message":
    {
        "type": "reset_channel_maintainer"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"reset_channel_maintainer"`. |

No additional body fields.

### Client request: `go_to_idle_mode_request`

**Body**

```json
{
    "message":
    {
        "type": "go_to_idle_mode_request"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"go_to_idle_mode_request"`. |

No additional body fields.

### Client request: `come_back_from_idle_mode_request`

**Body**

```json
{
    "message":
    {
        "type": "come_back_from_idle_mode_request",
        "channel_id": 3,
        "channel_password": ""
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"come_back_from_idle_mode_request"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.channel_password` | string | Optional; absent means no supplied password | Channel password; `""` means no password. Shorter than `CHANNEL_PASSWORD_MAX_LENGTH` (128). |

### Client request: `call_idle_client_request`

**Body**

```json
{
    "message":
    {
        "type": "call_idle_client_request",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"call_idle_client_request"`. |
| `message.client_id` | number | Required | Client ID of the idle recipient. |

### Server response: `channel_list`

Each entry omits `maintainer_id`, `channel_creator_id`, passwords and encryption keys.

**Body**

```json
{
    "message":
    {
        "type": "channel_list",
        "channels":
        [
            {
                "channel_id": 3,
                "parent_channel_id": 0,
                "name": "General",
                "description": "General discussion",
                "is_using_password": false,
                "is_audio_enabled": true,
                "is_video_stream_enabled": true,
                "is_root_channel": false,
                "has_maintainer": true,
                "is_temp_channel": false,
                "is_client_limit_active": true,
                "max_client_count": 20,
                "has_channel_icon": true,
                "channel_icon_id": 4,
                "required_talk_power": 25,
                "required_message_power": 25,
                "required_picture_power": 25,
                "required_stream_power": 25
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_list"`. |
| `message.channels` | object[] | Required | Channel entries; `[]` is an empty list. |
| `message.channels[].channel_id` | number | Required | Numeric channel ID. |
| `message.channels[].parent_channel_id` | number | Required | Parent channel ID. |
| `message.channels[].name` | string | Required | Channel display name. The list uses `name`, not `channel_name`. |
| `message.channels[].description` | string | Required | Channel description. The list uses `description`, not `channel_description`. |
| `message.channels[].is_using_password` | boolean | Required | Whether a password is configured; the password itself is absent. |
| `message.channels[].is_audio_enabled` | boolean | Required | Whether channel audio is enabled. |
| `message.channels[].is_video_stream_enabled` | boolean | Required | Whether channel video is enabled. |
| `message.channels[].is_root_channel` | boolean | Required | Whether this is the root channel. |
| `message.channels[].has_maintainer` | boolean | Required | Whether this channel currently has a key maintainer. |
| `message.channels[].is_temp_channel` | boolean | Required | Whether the channel is temporary. |
| `message.channels[].is_client_limit_active` | boolean | Required | Whether the client count limit is active. |
| `message.channels[].max_client_count` | number | Required | Configured occupancy limit; meaningful when the limit is active. |
| `message.channels[].has_channel_icon` | boolean | Required | Whether `channel_icon_id` identifies an assigned icon. |
| `message.channels[].channel_icon_id` | number | Required | Assigned server icon ID; meaningful only when `has_channel_icon` is true. |
| `message.channels[].required_talk_power` | number | Required | Minimum talk power. Whole number from `0` through `1000`. |
| `message.channels[].required_message_power` | number | Required | Minimum channel-text power. Whole number from `0` through `1000`. |
| `message.channels[].required_picture_power` | number | Required | Minimum channel-picture power. Whole number from `0` through `1000`. |
| `message.channels[].required_stream_power` | number | Required | Minimum video-stream power. Whole number from `0` through `1000`. |

### Server response: `channel_create`

**Body**

```json
{
    "message":
    {
        "type": "channel_create",
        "channel_id": 3,
        "parent_channel_id": 0,
        "name": "General",
        "description": "General discussion",
        "is_using_password": false,
        "is_audio_enabled": true,
        "is_video_stream_enabled": true,
        "is_root_channel": false,
        "has_maintainer": true,
        "is_temp_channel": false,
        "is_client_limit_active": true,
        "max_client_count": 20,
        "has_channel_icon": true,
        "channel_icon_id": 4,
        "required_talk_power": 25,
        "required_message_power": 25,
        "required_picture_power": 25,
        "required_stream_power": 25,
        "maintainer_id": 7,
        "channel_creator_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_create"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.parent_channel_id` | number | Required | Parent channel ID. |
| `message.name` | string | Required | Channel display name. The list uses `name`, not `channel_name`. |
| `message.description` | string | Required | Channel description. The list uses `description`, not `channel_description`. |
| `message.is_using_password` | boolean | Required | Whether a password is configured; the password itself is absent. |
| `message.is_audio_enabled` | boolean | Required | Whether channel audio is enabled. |
| `message.is_video_stream_enabled` | boolean | Required | Whether channel video is enabled. |
| `message.is_root_channel` | boolean | Required | Whether this is the root channel. |
| `message.has_maintainer` | boolean | Required | Whether this channel currently has a key maintainer. |
| `message.is_temp_channel` | boolean | Required | Whether the channel is temporary. |
| `message.is_client_limit_active` | boolean | Required | Whether the client count limit is active. |
| `message.max_client_count` | number | Required | Configured occupancy limit; meaningful when the limit is active. |
| `message.has_channel_icon` | boolean | Required | Whether `channel_icon_id` identifies an assigned icon. |
| `message.channel_icon_id` | number | Required | Assigned server icon ID; meaningful only when `has_channel_icon` is true. |
| `message.required_talk_power` | number | Required | Minimum talk power. Whole number from `0` through `1000`. |
| `message.required_message_power` | number | Required | Minimum channel-text power. Whole number from `0` through `1000`. |
| `message.required_picture_power` | number | Required | Minimum channel-picture power. Whole number from `0` through `1000`. |
| `message.required_stream_power` | number | Required | Minimum video-stream power. Whole number from `0` through `1000`. |
| `message.maintainer_id` | number | Required | Current maintainer client ID; meaningful only if `has_maintainer` is true. |
| `message.channel_creator_id` | number | Required | Client ID of the channel creator. |

### Server response: `channel_edit`

This body uses `channel_name` and `channel_description`. It has no parent, root, creator or maintainer fields.

**Body**

```json
{
    "message":
    {
        "type": "channel_edit",
        "channel_id": 3,
        "channel_editor_id": 7,
        "channel_name": "General",
        "channel_description": "General discussion",
        "is_using_password": false,
        "is_audio_enabled": true,
        "is_video_stream_enabled": true,
        "is_temp_channel": false,
        "is_client_limit_active": true,
        "max_client_count": 20,
        "has_channel_icon": true,
        "channel_icon_id": 4,
        "required_talk_power": 25,
        "required_message_power": 25,
        "required_picture_power": 25,
        "required_stream_power": 25
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_edit"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.channel_editor_id` | number | Required | Client ID of the editor. |
| `message.channel_name` | string | Required | Nonempty channel display name, at most 50 UTF-8 characters. |
| `message.channel_description` | string | Required | Channel description; an empty string is allowed. Shorter than `CHANNEL_DESCRIPTION_MAX_LENGTH` (1000). |
| `message.is_using_password` | boolean | Required | Whether a password is configured; the password itself is absent. |
| `message.is_audio_enabled` | boolean | Required | Whether channel audio is enabled. |
| `message.is_video_stream_enabled` | boolean | Required | Whether channel video is enabled. |
| `message.is_temp_channel` | boolean | Required | Whether the channel is temporary. |
| `message.is_client_limit_active` | boolean | Required | Whether the client count limit is active. |
| `message.max_client_count` | number | Required | Configured occupancy limit; meaningful when the limit is active. |
| `message.has_channel_icon` | boolean | Required | Whether `channel_icon_id` identifies an assigned icon. |
| `message.channel_icon_id` | number | Required | Assigned server icon ID; meaningful only when `has_channel_icon` is true. |
| `message.required_talk_power` | number | Required | Minimum talk power. Whole number from `0` through `1000`. |
| `message.required_message_power` | number | Required | Minimum channel-text power. Whole number from `0` through `1000`. |
| `message.required_picture_power` | number | Required | Minimum channel-picture power. Whole number from `0` through `1000`. |
| `message.required_stream_power` | number | Required | Minimum video-stream power. Whole number from `0` through `1000`. |

### Server response: `channel_join`

**Body**

```json
{
    "message":
    {
        "type": "channel_join",
        "channel_id": 3,
        "channel_group_id": 0,
        "client_id": 7,
        "audio_state": 2,
        "is_streaming_song": false,
        "song_name": ""
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_join"`. |
| `message.channel_id` | number | Required | Visible destination channel ID; `-1` can conceal membership. |
| `message.reason` | string | Optional | `"move"` for a forced move, including channel deletion; `"kick"` for a channel kick. Absent for a voluntary join. Also included when the destination is hidden. |
| `message.channel_group_id` | number | Required | Channel role ID: `0` ordinary member, `1` Channel Admin, `2`–`127` a custom role. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.audio_state` | number | Required | `1` transmitting; `2` push-to-talk enabled; `3` microphone disabled, reception enabled; `4` all audio disabled. |
| `message.is_streaming_song` | boolean | Required | Whether the joining client is streaming a song. |
| `message.song_name` | string | Required | Song display name; `""` when no song name is set. |

### Server response: `channel_maintainer_id`

**Body**

```json
{
    "message":
    {
        "type": "channel_maintainer_id",
        "channel_id": 3,
        "maintainer_id": 7,
        "maintainer_generation": 12,
        "has_maintainer": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_maintainer_id"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.maintainer_id` | number | Required | Maintainer client ID; meaningful only when `has_maintainer` is true. |
| `message.maintainer_generation` | number | Required | Numeric generation of the maintainer assignment. |
| `message.has_maintainer` | boolean | Required | Whether a maintainer currently exists. |

### Server response: `channel_delete`

**Body**

```json
{
    "message":
    {
        "type": "channel_delete",
        "channel_id": 3,
        "channel_deletor_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_delete"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.channel_deletor_id` | number or null | Required | Deleting client ID, or `null` when there is no client actor. The wire spelling is `deletor`. |

**No client actor**

```json
{
    "message":
    {
        "type": "channel_delete",
        "channel_id": 3,
        "channel_deletor_id": null
    }
}
```

### Server response: `channel_icon_changed`

**Body**

```json
{
    "message":
    {
        "type": "channel_icon_changed",
        "channel_id": 3,
        "has_channel_icon": true,
        "channel_icon_id": 4
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_icon_changed"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.has_channel_icon` | boolean | Required | Whether an icon is assigned. |
| `message.channel_icon_id` | number | Required | Icon ID; meaningful only when `has_channel_icon` is true. |

### Server response: `channel_full`

**Body**

```json
{
    "message":
    {
        "type": "channel_full",
        "channel_id": 3
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_full"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |

### Server response: `client_going_to_idle_mode`

**Body**

```json
{
    "message":
    {
        "type": "client_going_to_idle_mode",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_going_to_idle_mode"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Server response: `client_coming_back_from_idle_mode`

**Body**

```json
{
    "message":
    {
        "type": "client_coming_back_from_idle_mode",
        "client_id": 7,
        "channel_id": 3,
        "channel_group_id": 0
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_coming_back_from_idle_mode"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.channel_group_id` | number | Required | Channel role ID: `0` ordinary member, `1` Channel Admin, `2`–`127` a custom role. |

### Server response: `call`

**Body**

```json
{
    "message":
    {
        "type": "call",
        "caller_client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"call"`. |
| `message.caller_client_id` | number | Required | Client ID of the caller. |

## Chat and URL preview bodies

| Direction | Type |
| --- | --- |
| Client request | [`direct_chat_message`](#client-request-direct_chat_message) |
| Client request | [`channel_chat_message`](#client-request-channel_chat_message) |
| Client request | [`offline_chat_message`](#client-request-offline_chat_message) |
| Client request | [`delete_chat_message_request`](#client-request-delete_chat_message_request) |
| Client request | [`edit_chat_message_request`](#client-request-edit_chat_message_request) |
| Client request | [`poke_client`](#client-request-poke_client) |
| Client request | [`typing_indicator_request`](#client-request-typing_indicator_request) |
| Client request | [`resolve_url`](#client-request-resolve_url) |
| Server response | [`server_chat_message_id_for_local_message_id`](#server-response-server_chat_message_id_for_local_message_id) |
| Server response | [`direct_chat_message`](#server-response-direct_chat_message) |
| Server response | [`channel_chat_message`](#server-response-channel_chat_message) |
| Server response | [`offline_chat_message`](#server-response-offline_chat_message) |
| Server response | [`chat_message_delete`](#server-response-chat_message_delete) |
| Server response | [`chat_message_edit`](#server-response-chat_message_edit) |
| Server response | [`poke`](#server-response-poke) |
| Server response | [`typing_indicator`](#server-response-typing_indicator) |
| Server response | [`url_resolved`](#server-response-url_resolved) |

### Client request: `direct_chat_message`

**Server visibility:** `receiver_id`, `local_message_id` and `type` are readable metadata. `value` is encrypted content; the server cannot read the direct-message plaintext.

**Body**

```json
{
    "message":
    {
        "type": "direct_chat_message",
        "value": "{\"message_keys\":\"<RSA-encrypted keys>\",\"message_value\":\"{\\\"iv\\\":\\\"<base64 IV>\\\",\\\"data\\\":\\\"<base64 ciphertext>\\\"}\"}",
        "receiver_id": 8,
        "local_message_id": 41
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"direct_chat_message"`. |
| `message.value` | string | Required | Serialized end-to-end encrypted direct-content object. Its plaintext is not visible to the server; see [Direct encrypted envelope](#direct-encrypted-envelope). Nonempty; maximum length is `(websocket_message_max_length * 3 / 4) - 2048`, using the server UTF-8 length check. |
| `message.receiver_id` | number | Required | Recipient client ID for a direct conversation. |
| `message.local_message_id` | number | Required | Sender-assigned local message ID. This is a JSON number, separate from the server message ID. |

### Client request: `channel_chat_message`

**Body**

```json
{
    "message":
    {
        "type": "channel_chat_message",
        "value": "{\"iv\":\"<base64 IV>\",\"data\":\"<base64 ciphertext>\"}",
        "receiver_id": 3,
        "local_message_id": 41
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_chat_message"`. |
| `message.value` | string | Required | Serialized encrypted channel-text envelope containing `iv` and `data`. Nonempty; maximum length is `(websocket_message_max_length * 3 / 4) - 2048`, using the server UTF-8 length check. |
| `message.receiver_id` | number | Required | Required numeric field. The channel is the sender's current channel; this value does not select a different channel. |
| `message.local_message_id` | number | Required | Sender-assigned local message ID. This is a JSON number, separate from the server message ID. |

### Client request: `offline_chat_message`

**Body**

```json
{
    "message":
    {
        "type": "offline_chat_message",
        "recipient_identity_hash": "<base64 identity hash>",
        "value": "{\"message_keys\":\"<RSA-encrypted keys>\",\"message_value\":\"{\\\"iv\\\":\\\"<base64 IV>\\\",\\\"data\\\":\\\"<base64 ciphertext>\\\"}\"}"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"offline_chat_message"`. |
| `message.recipient_identity_hash` | string | Required | Nonempty stored recipient identity hash; fewer than 64 UTF-8 characters. |
| `message.value` | string | Required | Serialized end-to-end encrypted direct-content object for the stored recipient. The server stores ciphertext, not readable message content. Nonempty and fewer than 8192 UTF-8 characters. |

### Client request: `delete_chat_message_request`

**Body**

```json
{
    "message":
    {
        "type": "delete_chat_message_request",
        "message_id": 900,
        "receiver_type": "user",
        "receiver_id": 8
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"delete_chat_message_request"`. |
| `message.message_id` | number | Required | Server chat message ID of the affected message. |
| `message.receiver_type` | string | Required | Exactly `"user"` or `"channel"`. |
| `message.receiver_id` | number | Required when receiver_type is user | Direct conversation partner's client ID. Not required for a channel body. |

**Channel body**

```json
{
    "message":
    {
        "type": "delete_chat_message_request",
        "message_id": 900,
        "receiver_type": "channel"
    }
}
```

### Client request: `edit_chat_message_request`

**Legacy, insecure body.** Retained here because the server still accepts it. The updated client does not send it; use the [encrypted edit control body](#edit-control-body) instead.

For `receiver_type: "user"`, this legacy request requires private messaging to be enabled and a live, authenticated, non-bot recipient. An idle recipient also requires the server's idle-text setting and the sender's `message_idle_clients` permission. A policy refusal returns `access_denied`; these sending restrictions do not prevent deletion of an existing message.

**Body**

```json
{
    "message":
    {
        "type": "edit_chat_message_request",
        "message_id": 900,
        "receiver_type": "user",
        "receiver_id": 8,
        "new_message_value": "Corrected message"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"edit_chat_message_request"`. |
| `message.message_id` | number | Required | Server chat message ID of the affected message. |
| `message.receiver_type` | string | Required | Exactly `"user"` or `"channel"`. |
| `message.receiver_id` | number | Required when receiver_type is user | Direct conversation partner's client ID. Not required for a channel body. |
| `message.new_message_value` | string | Required | Nonempty replacement text, **readable by the server after transport decryption**, including direct-message edits. It is not inside the end-to-end encrypted `value` envelope. Maximum length is `(websocket_message_max_length * 3 / 4) - 2048`, using the server UTF-8 length check. |

**Channel body**

```json
{
    "message":
    {
        "type": "edit_chat_message_request",
        "message_id": 900,
        "receiver_type": "channel",
        "new_message_value": "Corrected message"
    }
}
```

### Client request: `poke_client`

**Body**

```json
{
    "message":
    {
        "type": "poke_client",
        "client_id": 7,
        "poke_message": "Please check the channel."
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"poke_client"`. |
| `message.client_id` | number | Required | Client ID of the poke recipient. |
| `message.poke_message` | string | Required | Nonempty text of the poke. |

### Client request: `typing_indicator_request`

**Body**

```json
{
    "message":
    {
        "type": "typing_indicator_request",
        "receiver_type": "user",
        "receiver_id": 8
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"typing_indicator_request"`. |
| `message.receiver_type` | string | Required | Exactly `"user"` or `"channel"`. |
| `message.receiver_id` | number | Required | Client ID for `user`; channel context for `channel`. |

### Client request: `resolve_url`

**Body**

```json
{
    "message":
    {
        "type": "resolve_url",
        "url": "https://example.com/article",
        "request_id": "preview-41"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"resolve_url"`. |
| `message.url` | string | Required | URL string accepted by the HTTP URL parser, at most 2048 characters. |
| `message.request_id` | string | Required | Nonempty caller-provided correlation string, at most 64 characters. |

### Server response: `server_chat_message_id_for_local_message_id`

**Body**

```json
{
    "message":
    {
        "type": "server_chat_message_id_for_local_message_id",
        "server_chat_message_id": 900,
        "local_message_id": 41
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_chat_message_id_for_local_message_id"`. |
| `message.server_chat_message_id` | number | Required | Server-assigned chat message ID, serialized as a JSON number. |
| `message.local_message_id` | number | Required | Sender-assigned local message ID. This is a JSON number, separate from the server message ID. |

### Server response: `direct_chat_message`

**Server visibility:** the server supplies `sender_username`, `sender_id` and `server_chat_message_id` and forwards the sender's encrypted `value` unchanged. This response contains no decrypted direct-message text.

**Body**

```json
{
    "message":
    {
        "type": "direct_chat_message",
        "value": "{\"message_keys\":\"<RSA-encrypted keys>\",\"message_value\":\"{\\\"iv\\\":\\\"<base64 IV>\\\",\\\"data\\\":\\\"<base64 ciphertext>\\\"}\"}",
        "sender_username": "Alice",
        "sender_id": 7,
        "server_chat_message_id": 900
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"direct_chat_message"`. |
| `message.value` | string | Required | Sender's end-to-end encrypted direct-content envelope, forwarded unchanged as a string. Only the recipient client decrypts the received content; the server cannot read its plaintext. |
| `message.sender_username` | string | Required | Author's display name. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.server_chat_message_id` | number | Required | Server-assigned chat message ID, serialized as a JSON number. |

### Server response: `channel_chat_message`

**Body**

```json
{
    "message":
    {
        "type": "channel_chat_message",
        "value": "{\"iv\":\"<base64 IV>\",\"data\":\"<base64 ciphertext>\"}",
        "sender_username": "Alice",
        "sender_id": 7,
        "server_chat_message_id": 900,
        "channel_id": 3
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_chat_message"`. |
| `message.value` | string | Required | Encrypted channel-text envelope serialized as a string. |
| `message.sender_username` | string | Required | Author's display name. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.server_chat_message_id` | number | Required | Server-assigned chat message ID, serialized as a JSON number. |
| `message.channel_id` | number | Required | Numeric channel ID. |

### Server response: `offline_chat_message`

**Body**

```json
{
    "message":
    {
        "type": "offline_chat_message",
        "value": "{\"message_keys\":\"<RSA-encrypted keys>\",\"message_value\":\"{\\\"iv\\\":\\\"<base64 IV>\\\",\\\"data\\\":\\\"<base64 ciphertext>\\\"}\"}",
        "sender_identity_hash": "<base64 identity hash>",
        "sender_name": "Alice",
        "queued_unix_seconds": 1793520000,
        "sequence_number": 12
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"offline_chat_message"`. |
| `message.value` | string | Required | Queued end-to-end encrypted direct-content envelope serialized as a string. The server does not decrypt the queued message content. |
| `message.sender_identity_hash` | string | Required | Stored author identity hash. |
| `message.sender_name` | string | Required | Stored author display name. |
| `message.queued_unix_seconds` | number | Required | Queue timestamp in Unix seconds, not milliseconds. |
| `message.sequence_number` | number | Required | Offline queue sequence number. |

### Server response: `chat_message_delete`

**Body**

```json
{
    "message":
    {
        "type": "chat_message_delete",
        "chat_message_id": 900,
        "requester_public_key": "<RSA public key>",
        "requester_is_admin": false
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"chat_message_delete"`. |
| `message.chat_message_id` | number | Required | Affected server chat message ID. This field is named differently from request `message_id`. |
| `message.requester_public_key` | string | Required | Public key of the client requesting the change. |
| `message.requester_is_admin` | boolean | Required | Whether the requester is an administrator. |

### Server response: `chat_message_edit`

**Legacy, insecure outer response.** The updated client ignores this body. It accepts edits only inside decrypted direct or channel chat content. The [encrypted edit control body](#edit-control-body) uses the same type spelling inside the content, with a different structure.

**Body**

```json
{
    "message":
    {
        "type": "chat_message_edit",
        "chat_message_id": 900,
        "requester_public_key": "<RSA public key>",
        "requester_is_admin": false,
        "new_message_value": "Corrected message"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"chat_message_edit"`. |
| `message.chat_message_id` | number | Required | Affected server chat message ID. This field is named differently from request `message_id`. |
| `message.requester_public_key` | string | Required | Public key of the client requesting the change. |
| `message.requester_is_admin` | boolean | Required | Whether the requester is an administrator. |
| `message.new_message_value` | string | Required | Nonempty replacement text forwarded by the server, **readable by the server after transport decryption**, including direct-message edits. It is not inside the end-to-end encrypted `value` envelope. Maximum length is `(websocket_message_max_length * 3 / 4) - 2048`, using the server UTF-8 length check. |

### Server response: `poke`

**Body**

```json
{
    "message":
    {
        "type": "poke",
        "client_id": 7,
        "poke_message": "Please check the channel."
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"poke"`. |
| `message.client_id` | number | Required | Client ID of the poke sender, not the recipient. |
| `message.poke_message` | string | Required | Nonempty text of the poke. |

### Server response: `typing_indicator`

**Body**

```json
{
    "message":
    {
        "type": "typing_indicator",
        "client_id": 7,
        "receiver_type": "user",
        "receiver_id": 8
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"typing_indicator"`. |
| `message.client_id` | number | Required | Client ID of the person typing. |
| `message.receiver_type` | string | Required | Exactly `"user"` or `"channel"`. |
| `message.receiver_id` | number | Required | Conversation identifier: client or channel ID according to `receiver_type`. |

### Server response: `url_resolved`

**Body**

```json
{
    "message":
    {
        "type": "url_resolved",
        "request_id": "preview-41",
        "url": "https://example.com/article",
        "status": "ok",
        "resolved_url": "https://example.com/article",
        "title": "Example article",
        "description": "An example page.",
        "site_name": "Example",
        "image": "data:image/png;base64,<image bytes>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"url_resolved"`. |
| `message.request_id` | string | Required | Nonempty caller-provided correlation string, at most 64 characters. |
| `message.url` | string | Required | Original URL from the request. |
| `message.status` | string | Required | Exactly `"ok"`, `"unavailable"` or `"disabled"`. |
| `message.resolved_url` | string | Present when status is ok | Final URL of the preview. |
| `message.title` | string | Present when status is ok | Preview title; can be empty. |
| `message.description` | string | Present when status is ok | Preview description; can be empty. |
| `message.site_name` | string | Present when status is ok | Preview site name; can be empty. |
| `message.image` | string | Optional on ok | Preview image data URL. Omitted when no image was resolved. |

**Unavailable preview**

```json
{
    "message":
    {
        "type": "url_resolved",
        "request_id": "preview-41",
        "url": "https://example.com/article",
        "status": "unavailable"
    }
}
```

**Disabled preview**

```json
{
    "message":
    {
        "type": "url_resolved",
        "request_id": "preview-41",
        "url": "https://example.com/article",
        "status": "disabled"
    }
}
```

## Picture and file bodies

| Direction | Type |
| --- | --- |
| Client request | [`set_attachment_receive_preferences`](#client-request-set_attachment_receive_preferences) |
| Client request | [`file_send`](#client-request-file_send) |
| Client request | [`file_send_completed`](#client-request-file_send_completed) |
| Server response | [`file_send_success`](#server-response-file_send_success) |
| Server response | [`file_send_error`](#server-response-file_send_error) |
| Server response | [`image_sent_status`](#server-response-image_sent_status) |
| Server response | [`direct_chat_picture_metadata`](#server-response-direct_chat_picture_metadata) |
| Server response | [`channel_chat_picture_metadata`](#server-response-channel_chat_picture_metadata) |
| Server response | [`direct_chat_file_metadata`](#server-response-direct_chat_file_metadata) |
| Server response | [`channel_chat_file_metadata`](#server-response-channel_chat_file_metadata) |
| Server response | [`file_receive_chunk`](#server-response-file_receive_chunk) |
| Server response | [`file_receive_completed`](#server-response-file_receive_completed) |
| Server response | [`channel_chat_picture`](#server-response-channel_chat_picture) |

### Client request: `set_attachment_receive_preferences`

**Body**

```json
{
    "message":
    {
        "type": "set_attachment_receive_preferences",
        "block_direct_chat_pictures": false,
        "block_direct_chat_files": false,
        "block_channel_chat_pictures": false,
        "block_channel_chat_files": false
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"set_attachment_receive_preferences"`. |
| `message.block_direct_chat_pictures` | boolean | Required | Whether this client blocks incoming direct-chat pictures. |
| `message.block_direct_chat_files` | boolean | Required | Whether this client blocks incoming direct-chat files. |
| `message.block_channel_chat_pictures` | boolean | Required | Whether this client blocks incoming channel-chat pictures. |
| `message.block_channel_chat_files` | boolean | Required | Whether this client blocks incoming channel-chat files. |

### Client request: `file_send`

**Body**

```json
{
    "message":
    {
        "type": "file_send",
        "total_bytes_length": 8,
        "data_part_base64": "QUJDRA==",
        "is_new_file": true,
        "file_send_intent": "direct_chat_file"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"file_send"`. |
| `message.total_bytes_length` | number | Required | Total length of the assembled encoded upload string, repeated on every chunk; not the raw file byte count. Must be greater than zero and strictly below the server upload-buffer limit. |
| `message.data_part_base64` | string | Required | This chunk of the encoded upload string. Maximum UTF-8 length is `max(16384, floor(file_transfer__get_max_file_upload_length() / 400) + 1)`. |
| `message.is_new_file` | boolean | Required | True for the first chunk; false for a continuation. |
| `message.file_send_intent` | string | Required on the first chunk | Exactly one of `"direct_chat_picture_file"`, `"channel_chat_picture_file"`, `"direct_chat_file"`, `"channel_chat_file"`, `"musicbot_file"`. |

**Continuation chunk**

```json
{
    "message":
    {
        "type": "file_send",
        "total_bytes_length": 16,
        "data_part_base64": "QUJDRA==",
        "is_new_file": false
    }
}
```

### Client request: `file_send_completed`

There is no single extra-data object containing every field. Each intent has the body shown below.

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"file_send_completed"`. |
| `message.file_send_intent` | string | Required | Exactly one of `"direct_chat_picture_file"`, `"channel_chat_picture_file"`, `"direct_chat_file"`, `"channel_chat_file"`, `"musicbot_file"`. Must match the upload intent. |
| `message.file_send_intent_extra_data` | object | Required | Intent-specific metadata object. The five complete body variants below show which members belong together. |
| `message.file_send_intent_extra_data.receiver_id` | number | Required for picture/file intents | Recipient client ID for a direct intent; channel context ID for a channel intent. |
| `message.file_send_intent_extra_data.local_message_id` | number | Required for picture/file intents | Sender-assigned local message ID. This is a JSON number, separate from the server message ID. |
| `message.file_send_intent_extra_data.file_header` | string | Required for direct_chat_file and channel_chat_file | Encrypted file-header string; nonempty and no longer than `FILE_HEADER_MAX_LENGTH` (8192). Direct files use a serialized direct-file envelope; channel files use a serialized [authenticated file envelope](#authenticated-file-envelope). Its decrypted body is documented under [File header](#file-header). |
| `message.file_send_intent_extra_data.song_name` | string | Required for musicbot_file | Nonempty song display name, at most 511 UTF-8 characters. |
| `message.file_send_intent_extra_data.musicbot_id` | number | Required for musicbot_file | Client ID of the target music bot. |

**direct_chat_picture_file**

```json
{
    "message":
    {
        "type": "file_send_completed",
        "file_send_intent": "direct_chat_picture_file",
        "file_send_intent_extra_data":
        {
            "receiver_id": 8,
            "local_message_id": 41
        }
    }
}
```

**channel_chat_picture_file**

```json
{
    "message":
    {
        "type": "file_send_completed",
        "file_send_intent": "channel_chat_picture_file",
        "file_send_intent_extra_data":
        {
            "receiver_id": 3,
            "local_message_id": 41
        }
    }
}
```

**direct_chat_file**

```json
{
    "message":
    {
        "type": "file_send_completed",
        "file_send_intent": "direct_chat_file",
        "file_send_intent_extra_data":
        {
            "receiver_id": 8,
            "local_message_id": 41,
            "file_header": "{\"message_keys\":\"<RSA-encrypted keys>\",\"message_value\":\"<base64 file ciphertext>\"}"
        }
    }
}
```

**channel_chat_file**

```json
{
    "message":
    {
        "type": "file_send_completed",
        "file_send_intent": "channel_chat_file",
        "file_send_intent_extra_data":
        {
            "receiver_id": 3,
            "local_message_id": 41,
            "file_header": "<base64 file-header ciphertext>"
        }
    }
}
```

**musicbot_file**

```json
{
    "message":
    {
        "type": "file_send_completed",
        "file_send_intent": "musicbot_file",
        "file_send_intent_extra_data":
        {
            "song_name": "Example track",
            "musicbot_id": 9
        }
    }
}
```

### Server response: `file_send_success`

**Body**

```json
{
    "message":
    {
        "type": "file_send_success"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"file_send_success"`. |

No additional body fields.

### Server response: `file_send_error`

**Body**

```json
{
    "message":
    {
        "type": "file_send_error",
        "reason": "receiver_blocked_attachment",
        "local_message_id": 41,
        "file_upload_max_size": 10485760
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"file_send_error"`. |
| `message.reason` | string | Required | Current codes: `file_too_large`, `file_uploads_disabled`, `insufficient_permissions`, `server_ram_budget_reached`, `server_busy`, `receiver_unavailable`, `receiver_blocked_attachment`, `private_messages_disabled`. |
| `message.local_message_id` | number | Required | Associated local message ID; can be `0` when unavailable. |
| `message.file_upload_max_size` | number | Required | Current chat-file size limit in bytes. |

### Server response: `image_sent_status`

**Body**

```json
{
    "message":
    {
        "type": "image_sent_status",
        "value": "success"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"image_sent_status"`. |
| `message.value` | string | Required | Current serializer value is exactly `"success"`. |

### Server response: `direct_chat_picture_metadata`

**Server visibility:** the sender fields, picture ID and encrypted upload length below are readable metadata. The picture content is encrypted separately and is not visible to the server.

**Body**

```json
{
    "message":
    {
        "type": "direct_chat_picture_metadata",
        "sender_username": "Alice",
        "sender_id": 7,
        "picture_id": 900,
        "size": 2048
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"direct_chat_picture_metadata"`. |
| `message.sender_username` | string | Required | Author's display name. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.picture_id` | number | Required | Server-assigned picture message ID, also used by the chunk body as `server_chat_message_id`. |
| `message.size` | number | Required | Length of the encrypted/encoded picture upload, not a pixel dimension. |

### Server response: `channel_chat_picture_metadata`

**Body**

```json
{
    "message":
    {
        "type": "channel_chat_picture_metadata",
        "sender_id": 7,
        "channel_id": 3,
        "picture_id": 900,
        "size": 2048
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_chat_picture_metadata"`. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.picture_id` | number | Required | Server-assigned picture message ID, also used by the chunk body as `server_chat_message_id`. |
| `message.size` | number | Required | Length of the encrypted/encoded picture upload, not a pixel dimension. |

### Server response: `direct_chat_file_metadata`

**Server visibility:** the sender fields, file ID and encrypted upload length are readable metadata. `file_header` and the file content remain encrypted; the server cannot read the filename, MIME type or original file size inside the header.

**Body**

```json
{
    "message":
    {
        "type": "direct_chat_file_metadata",
        "sender_username": "Alice",
        "sender_id": 7,
        "file_id": 900,
        "file_header": "{\"message_keys\":\"<RSA-encrypted keys>\",\"message_value\":\"<base64 file ciphertext>\"}",
        "encrypted_size": 2048
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"direct_chat_file_metadata"`. |
| `message.sender_username` | string | Required | Author's display name. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.file_id` | number | Required | Server-assigned file message ID, also used by chunks as `server_chat_message_id`. |
| `message.file_header` | string | Required | Encrypted file-header string. The filename, MIME type and raw size are inside its decrypted body. |
| `message.encrypted_size` | number | Required | Length of the encrypted/encoded upload string, not the original file size. |

### Server response: `channel_chat_file_metadata`

**Body**

```json
{
    "message":
    {
        "type": "channel_chat_file_metadata",
        "channel_id": 3,
        "sender_id": 7,
        "file_id": 900,
        "file_header": "<base64 file-header ciphertext>",
        "encrypted_size": 2048
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_chat_file_metadata"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.file_id` | number | Required | Server-assigned file message ID, also used by chunks as `server_chat_message_id`. |
| `message.file_header` | string | Required | Encrypted file-header string. The filename, MIME type and raw size are inside its decrypted body. |
| `message.encrypted_size` | number | Required | Length of the encrypted/encoded upload string, not the original file size. |

### Server response: `file_receive_chunk`

**Body**

```json
{
    "message":
    {
        "type": "file_receive_chunk",
        "value": "<encoded chunk>",
        "sender_id": 7,
        "server_chat_message_id": 900
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"file_receive_chunk"`. |
| `message.value` | string | Required | One string segment of the encrypted/encoded attachment. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.server_chat_message_id` | number | Required | Server-assigned chat message ID, serialized as a JSON number. |

### Server response: `file_receive_completed`

**Body**

```json
{
    "message":
    {
        "type": "file_receive_completed",
        "receive_type": "direct_chat_file",
        "sender_id": 7,
        "server_chat_message_id": 900
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"file_receive_completed"`. |
| `message.receive_type` | string | Required | Exactly `"direct_chat_picture"`, `"channel_chat_picture"`, `"direct_chat_file"` or `"channel_chat_file"`. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.server_chat_message_id` | number | Required | Server-assigned chat message ID, serialized as a JSON number. |

### Server response: `channel_chat_picture`

A serializer for this body exists, but the current attachment path uses metadata, chunk and completion bodies. This is retained as an explicitly identified dormant wire shape.

**Body**

```json
{
    "message":
    {
        "type": "channel_chat_picture",
        "value": "<encrypted picture>",
        "username": "Alice",
        "sender_id": 7,
        "channel_id": 3,
        "picture_id": 900
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_chat_picture"`. |
| `message.value` | string | Required | Complete encrypted picture value as a string. |
| `message.username` | string | Required | Author display name. This body uses `username`, not `sender_username`. |
| `message.sender_id` | number | Required | Client ID of the author. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.picture_id` | number | Required | Server-assigned picture message ID, also used by the chunk body as `server_chat_message_id`. |

## Profiles and stored identities

| Direction | Type |
| --- | --- |
| Client request | [`change_client_username`](#client-request-change_client_username) |
| Client request | [`request_stored_clients`](#client-request-request_stored_clients) |
| Client request | [`avatar_upload`](#client-request-avatar_upload) |
| Client request | [`delete_avatar`](#client-request-delete_avatar) |
| Client request | [`request_avatar_for_client`](#client-request-request_avatar_for_client) |
| Client request | [`request_avatars`](#client-request-request_avatars) |
| Server response | [`client_rename`](#server-response-client_rename) |
| Server response | [`client_country_code_changed`](#server-response-client_country_code_changed) |
| Server response | [`avatar_changed`](#server-response-avatar_changed) |
| Server response | [`client_avatar`](#server-response-client_avatar) |
| Server response | [`stored_clients_list`](#server-response-stored_clients_list) |

### Client request: `change_client_username`

**Body**

```json
{
    "message":
    {
        "type": "change_client_username",
        "client_id": 7,
        "new_username": "Alice 2"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"change_client_username"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.new_username` | string | Required | Replacement display name, 1–50 UTF-8 characters. |

### Client request: `request_stored_clients`

**Body**

```json
{
    "message":
    {
        "type": "request_stored_clients"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"request_stored_clients"`. |

No additional body fields.

### Client request: `avatar_upload`

**Body**

```json
{
    "message":
    {
        "type": "avatar_upload",
        "base64_avatar": "<base64 avatar>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"avatar_upload"`. |
| `message.base64_avatar` | string | Required | Base64 avatar string. Nonempty, below `MAX_CLIENT_AVATAR_LENGTH`, and no longer than `(avatar_max_size_bytes * 4 / 3) + 64`. |

### Client request: `delete_avatar`

**Body**

```json
{
    "message":
    {
        "type": "delete_avatar"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"delete_avatar"`. |

No additional body fields.

### Client request: `request_avatar_for_client`

**Body**

```json
{
    "message":
    {
        "type": "request_avatar_for_client",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"request_avatar_for_client"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Client request: `request_avatars`

**Body**

```json
{
    "message":
    {
        "type": "request_avatars",
        "client_ids":
        [7, 8]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"request_avatars"`. |
| `message.client_ids` | number[] | Required | Client IDs whose avatars are requested. IDs are client-slot numbers within server capacity; the batch is capped at 100 returned avatars. |

### Server response: `client_rename`

**Body**

```json
{
    "message":
    {
        "type": "client_rename",
        "client_id": 7,
        "new_username": "Alice 2"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_rename"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.new_username` | string | Required | Accepted replacement display name. |

### Server response: `client_country_code_changed`

**Body**

```json
{
    "message":
    {
        "type": "client_country_code_changed",
        "client_id": 7,
        "country_iso_code": "SK"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_country_code_changed"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.country_iso_code` | string | Required | Two-letter country code; `""` when unknown or hidden. |

### Server response: `avatar_changed`

**Body**

```json
{
    "message":
    {
        "type": "avatar_changed",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"avatar_changed"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Server response: `client_avatar`

**Body**

```json
{
    "message":
    {
        "type": "client_avatar",
        "client_id": 7,
        "base64_avatar": "<base64 avatar>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_avatar"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.base64_avatar` | string | Required | Base64 avatar content; `""` represents no avatar. |

**No avatar**

```json
{
    "message":
    {
        "type": "client_avatar",
        "client_id": 7,
        "base64_avatar": ""
    }
}
```

### Server response: `stored_clients_list`

**Body**

```json
{
    "message":
    {
        "type": "stored_clients_list",
        "stored_clients":
        [
            {
                "identity_hash": "<base64 identity hash>",
                "username": "Alice",
                "base64_avatar": "",
                "public_key": "<RSA public key>",
                "last_seen": 1793520000,
                "group_ids":
                [2]
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"stored_clients_list"`. |
| `message.stored_clients` | object[] | Required | Stored-user entries; may be an empty array. |
| `message.stored_clients[].identity_hash` | string | Required | Base64 identity hash derived from the public key. |
| `message.stored_clients[].username` | string | Required | Client display name. |
| `message.stored_clients[].base64_avatar` | string | Required | Stored base64 avatar; `""` when absent. |
| `message.stored_clients[].public_key` | string | Conditional: offline messaging and permissions allow exposure, and the key is stored | RSA public-key representation as a string. |
| `message.stored_clients[].last_seen` | number | Conditional: last-seen data is enabled and nonzero | Last-seen timestamp in Unix seconds, not milliseconds. Omitted when disabled or zero. |
| `message.stored_clients[].group_ids` | number[] | Required | Server-group IDs; `[]` means no listed memberships. |

## Administration and server groups

| Direction | Type |
| --- | --- |
| Client request | [`admin_password`](#client-request-admin_password) |
| Client request | [`change_admin_password`](#client-request-change_admin_password) |
| Client request | [`add_group_to_client`](#client-request-add_group_to_client) |
| Client request | [`remove_group_from_client`](#client-request-remove_group_from_client) |
| Client request | [`server_settings_add_new_group`](#client-request-server_settings_add_new_group) |
| Client request | [`server_settings_delete_group`](#client-request-server_settings_delete_group) |
| Client request | [`rename_group`](#client-request-rename_group) |
| Client request | [`set_group_permissions`](#client-request-set_group_permissions) |
| Client request | [`save_groups`](#client-request-save_groups) |
| Client request | [`server_settings_icon_upload`](#client-request-server_settings_icon_upload) |
| Client request | [`server_settings_delete_icon`](#client-request-server_settings_delete_icon) |
| Client request | [`server_settings_set_group_icon`](#client-request-server_settings_set_group_icon) |
| Client request | [`request_identity_list`](#client-request-request_identity_list) |
| Client request | [`delete_identity`](#client-request-delete_identity) |
| Client request | [`modify_identity_group`](#client-request-modify_identity_group) |
| Client request | [`get_client_info`](#client-request-get_client_info) |
| Client request | [`admin_log_request`](#client-request-admin_log_request) |
| Client request | [`admin_log_clear`](#client-request-admin_log_clear) |
| Client request | [`kick`](#client-request-kick) |
| Client request | [`ban`](#client-request-ban) |
| Client request | [`remove_ban`](#client-request-remove_ban) |
| Server response | [`admin_status`](#server-response-admin_status) |
| Server response | [`force_admin_password_change`](#server-response-force_admin_password_change) |
| Server response | [`access_denied`](#server-response-access_denied) |
| Server response | [`client_info`](#server-response-client_info) |
| Server response | [`admin_log`](#server-response-admin_log) |
| Server response | [`icon_list`](#server-response-icon_list) |
| Server response | [`icon_add`](#server-response-icon_add) |
| Server response | [`icon_delete`](#server-response-icon_delete) |
| Server response | [`group_list`](#server-response-group_list) |
| Server response | [`group_add`](#server-response-group_add) |
| Server response | [`group_delete`](#server-response-group_delete) |
| Server response | [`group_add_to_client`](#server-response-group_add_to_client) |
| Server response | [`remove_group_from_client`](#server-response-remove_group_from_client) |
| Server response | [`group_icon_changed`](#server-response-group_icon_changed) |
| Server response | [`group_permissions_changed`](#server-response-group_permissions_changed) |
| Server response | [`identity_list`](#server-response-identity_list) |

### Client request: `admin_password`

**Body**

```json
{
    "message":
    {
        "type": "admin_password",
        "value": "<admin password>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"admin_password"`. |
| `message.value` | string | Required | Administrator password; nonempty, at most 50 UTF-8 bytes. |

### Client request: `change_admin_password`

**Body**

```json
{
    "message":
    {
        "type": "change_admin_password",
        "value": "<new admin password>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"change_admin_password"`. |
| `message.value` | string | Required | Replacement administrator password; nonempty, at most 50 UTF-8 bytes. |

### Client request: `add_group_to_client`

**Body**

```json
{
    "message":
    {
        "type": "add_group_to_client",
        "client_id": 7,
        "group_id": 2
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"add_group_to_client"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.group_id` | number | Required | Numeric server-group ID. |

### Client request: `remove_group_from_client`

**Body**

```json
{
    "message":
    {
        "type": "remove_group_from_client",
        "client_id": 7,
        "group_id": 2
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"remove_group_from_client"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.group_id` | number | Required | Numeric server-group ID. |

### Client request: `server_settings_add_new_group`

**Body**

```json
{
    "message":
    {
        "type": "server_settings_add_new_group",
        "group_name": "Members",
        "linked_icon_id": 4,
        "group_type": "normal"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_settings_add_new_group"`. |
| `message.group_name` | string | Required | Nonempty group name, fewer than 32 UTF-8 characters. |
| `message.linked_icon_id` | number | Optional | Existing server icon ID. Omission means no icon; explicit `null` is invalid. |
| `message.group_type` | string | Optional; defaults to normal | Exactly `"normal"`, `"guest"`, `"moderator"` or `"admin"`. |

### Client request: `server_settings_delete_group`

**Body**

```json
{
    "message":
    {
        "type": "server_settings_delete_group",
        "group_id": 2
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_settings_delete_group"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |

### Client request: `rename_group`

**Body**

```json
{
    "message":
    {
        "type": "rename_group",
        "group_id": 2,
        "group_name": "Members"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"rename_group"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.group_name` | string | Required | Nonempty group name, fewer than 32 bytes in its UTF-8 representation. |

### Client request: `set_group_permissions`

Each action is a stored string decision; numeric powers and `grants` are whole numbers from 0 through 1000. Input is a complete replacement definition: omitted actions, powers and grant thresholds take their defaults. Unknown names are invalid. The full list of supported members is expanded below.

This body supports all [53 server action permissions](permission-reference.md#complete-server-action-list) and [15 server power/protection fields](permission-reference.md#powers-and-protection). Their meanings and the [complete Grant/flag scope](permission-reference.md#complete-grant-and-flag-scope) are listed in the permission reference. Channel-group names are a separate subset and are documented with `edit_channel_group`.

**Body**

```json
{
    "message":
    {
        "type": "set_group_permissions",
        "group_id": 2,
        "permissions":
        {
            "create_channel": "default",
            "create_temp_channel": "default",
            "kick": "default",
            "ban": "default",
            "view_stored_users": "default",
            "view_group_permissions": "default",
            "view_group_members": "default",
            "create_groups": "default",
            "edit_groups": "default",
            "delete_groups": "default",
            "assign_groups": "default",
            "remove_groups": "default",
            "edit_group_permissions": "default",
            "upload_icons": "default",
            "view_client_details": "default",
            "view_server_logs": "default",
            "view_server_info": "default",
            "send_offline_messages": "default",
            "receive_offline_messages": "default",
            "message_idle_clients": "default",
            "call_idle_clients": "default",
            "delete_icons": "default",
            "upload_emojis": "default",
            "delete_emojis": "default",
            "view_ip_addresses": "default",
            "view_bans": "default",
            "remove_bans": "default",
            "move_clients": "default",
            "kick_from_channel": "default",
            "mute_clients": "default",
            "stop_client_streams": "default",
            "edit_channel_name": "grant",
            "edit_channel_description": "default",
            "edit_channel_password": "default",
            "edit_channel_powers": "default",
            "edit_channel_options": "default",
            "delete_channel": "default",
            "assign_channel_groups": "default",
            "send_direct_pictures": "default",
            "send_direct_files": "default",
            "send_channel_pictures": "default",
            "send_channel_files": "default",
            "create_music_bots": "default",
            "delete_music_bots": "default",
            "upload_music_tracks": "default",
            "delete_music_tracks": "default",
            "control_music_playback": "default",
            "use_reserved_slots": "default",
            "create_channel_groups": "default",
            "edit_channel_groups": "default",
            "delete_channel_groups": "default",
            "view_client_connection_time": "default",
            "bypass_channel_password": "default",
            "kick_power": 0,
            "kick_protection": 0,
            "ban_power": 0,
            "ban_protection": 0,
            "group_management_power": 0,
            "group_protection": 0,
            "talk_power": 25,
            "channel_message_power": 25,
            "channel_picture_power": 25,
            "stream_power": 25,
            "move_power": 0,
            "move_protection": 0,
            "moderation_power": 0,
            "moderation_protection": 0,
            "permission_edit_power": 0,
            "grants":
            {
                "create_channel": 1000,
                "create_temp_channel": 1000,
                "kick": 1000,
                "ban": 1000,
                "view_stored_users": 1000,
                "view_group_permissions": 1000,
                "view_group_members": 1000,
                "create_groups": 1000,
                "edit_groups": 1000,
                "delete_groups": 1000,
                "assign_groups": 1000,
                "remove_groups": 1000,
                "edit_group_permissions": 1000,
                "upload_icons": 1000,
                "view_client_details": 1000,
                "view_server_logs": 1000,
                "view_server_info": 1000,
                "send_offline_messages": 1000,
                "receive_offline_messages": 1000,
                "message_idle_clients": 1000,
                "call_idle_clients": 1000,
                "delete_icons": 1000,
                "upload_emojis": 1000,
                "delete_emojis": 1000,
                "view_ip_addresses": 1000,
                "view_bans": 1000,
                "remove_bans": 1000,
                "move_clients": 1000,
                "kick_from_channel": 1000,
                "mute_clients": 1000,
                "stop_client_streams": 1000,
                "edit_channel_name": 1000,
                "edit_channel_description": 1000,
                "edit_channel_password": 1000,
                "edit_channel_powers": 1000,
                "edit_channel_options": 1000,
                "delete_channel": 1000,
                "assign_channel_groups": 1000,
                "send_direct_pictures": 1000,
                "send_direct_files": 1000,
                "send_channel_pictures": 1000,
                "send_channel_files": 1000,
                "create_music_bots": 1000,
                "delete_music_bots": 1000,
                "upload_music_tracks": 1000,
                "delete_music_tracks": 1000,
                "control_music_playback": 1000,
                "use_reserved_slots": 1000,
                "create_channel_groups": 1000,
                "edit_channel_groups": 1000,
                "delete_channel_groups": 1000,
                "view_client_connection_time": 1000,
                "bypass_channel_password": 1000,
                "kick_power": 1000,
                "kick_protection": 1000,
                "ban_power": 1000,
                "ban_protection": 1000,
                "group_management_power": 1000,
                "group_protection": 1000,
                "talk_power": 1000,
                "channel_message_power": 1000,
                "channel_picture_power": 1000,
                "stream_power": 1000,
                "move_power": 1000,
                "move_protection": 1000,
                "moderation_power": 1000,
                "moderation_protection": 1000,
                "permission_edit_power": 1000
            },
            "flags":
            {
                "edit_channel_name":
                {
                    "negate": true
                }
            },
            "manage_icons": "default"
        }
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"set_group_permissions"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.permissions` | object | Required | Replacement server-group permission definition. All supported fields are listed below. |
| `message.permissions.create_channel` | string | Optional; defaults to default | Stored permission for `create_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_temp_channel` | string | Optional; defaults to default | Stored permission for `create_temp_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.kick` | string | Optional; defaults to default | Stored permission for `kick`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.ban` | string | Optional; defaults to default | Stored permission for `ban`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_stored_users` | string | Optional; defaults to default | Stored permission for `view_stored_users`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_group_permissions` | string | Optional; defaults to default | Stored permission for `view_group_permissions`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_group_members` | string | Optional; defaults to default | Stored permission for `view_group_members`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_groups` | string | Optional; defaults to default | Stored permission for `create_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_groups` | string | Optional; defaults to default | Stored permission for `edit_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_groups` | string | Optional; defaults to default | Stored permission for `delete_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.assign_groups` | string | Optional; defaults to default | Stored permission for `assign_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.remove_groups` | string | Optional; defaults to default | Stored permission for `remove_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_group_permissions` | string | Optional; defaults to default | Stored permission for `edit_group_permissions`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.upload_icons` | string | Optional; defaults to default | Stored permission for `upload_icons`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_client_details` | string | Optional; defaults to default | Stored permission for `view_client_details`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_server_logs` | string | Optional; defaults to default | Stored permission for `view_server_logs`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_server_info` | string | Optional; defaults to default | Stored permission for `view_server_info`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_offline_messages` | string | Optional; defaults to default | Stored permission for `send_offline_messages`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.receive_offline_messages` | string | Optional; defaults to default | Stored permission for `receive_offline_messages`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.message_idle_clients` | string | Optional; defaults to default | Stored permission for `message_idle_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.call_idle_clients` | string | Optional; defaults to default | Stored permission for `call_idle_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_icons` | string | Optional; defaults to default | Stored permission for `delete_icons`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.upload_emojis` | string | Optional; defaults to default | Stored permission for `upload_emojis`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_emojis` | string | Optional; defaults to default | Stored permission for `delete_emojis`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_ip_addresses` | string | Optional; defaults to default | Stored permission for `view_ip_addresses`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_bans` | string | Optional; defaults to default | Stored permission for `view_bans`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.remove_bans` | string | Optional; defaults to default | Stored permission for `remove_bans`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.move_clients` | string | Optional; defaults to default | Stored permission for `move_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.kick_from_channel` | string | Optional; defaults to default | Stored permission for `kick_from_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.mute_clients` | string | Optional; defaults to default | Stored permission for `mute_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.stop_client_streams` | string | Optional; defaults to default | Stored permission for `stop_client_streams`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_name` | string | Optional; defaults to default | Stored permission for `edit_channel_name`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_description` | string | Optional; defaults to default | Stored permission for `edit_channel_description`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_password` | string | Optional; defaults to default | Stored permission for `edit_channel_password`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_powers` | string | Optional; defaults to default | Stored permission for `edit_channel_powers`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_options` | string | Optional; defaults to default | Stored permission for `edit_channel_options`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_channel` | string | Optional; defaults to default | Stored permission for `delete_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.assign_channel_groups` | string | Optional; defaults to default | Stored permission for `assign_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_direct_pictures` | string | Optional; defaults to default | Stored permission for `send_direct_pictures`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_direct_files` | string | Optional; defaults to default | Stored permission for `send_direct_files`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_channel_pictures` | string | Optional; defaults to default | Stored permission for `send_channel_pictures`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_channel_files` | string | Optional; defaults to default | Stored permission for `send_channel_files`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_music_bots` | string | Optional; defaults to default | Stored permission for `create_music_bots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_music_bots` | string | Optional; defaults to default | Stored permission for `delete_music_bots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.upload_music_tracks` | string | Optional; defaults to default | Stored permission for `upload_music_tracks`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_music_tracks` | string | Optional; defaults to default | Stored permission for `delete_music_tracks`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.control_music_playback` | string | Optional; defaults to default | Stored permission for `control_music_playback`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.use_reserved_slots` | string | Optional; defaults to default | Stored permission for `use_reserved_slots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_channel_groups` | string | Optional; defaults to default | Stored permission for `create_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_groups` | string | Optional; defaults to default | Stored permission for `edit_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_channel_groups` | string | Optional; defaults to default | Stored permission for `delete_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_client_connection_time` | string | Optional; defaults to default | Stored permission for `view_client_connection_time`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.bypass_channel_password` | string | Optional; defaults to default | Stored permission for `bypass_channel_password`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.kick_power` | number | Optional; defaults to 0 | Power to kick a client from the server. Whole number `0`–`1000`. |
| `message.permissions.kick_protection` | number | Optional; defaults to 0 | Protection against being kicked from the server. Whole number `0`–`1000`. |
| `message.permissions.ban_power` | number | Optional; defaults to 0 | Power to ban a client from the server. Whole number `0`–`1000`. |
| `message.permissions.ban_protection` | number | Optional; defaults to 0 | Protection against a server ban. Whole number `0`–`1000`. |
| `message.permissions.group_management_power` | number | Optional; defaults to 0 | Power to manage server groups and memberships. Whole number `0`–`1000`. |
| `message.permissions.group_protection` | number | Optional; defaults to 0 | Protection threshold of the server group. Whole number `0`–`1000`. |
| `message.permissions.talk_power` | number | Optional; defaults to 25 | Power to transmit channel voice. Whole number `0`–`1000`. |
| `message.permissions.channel_message_power` | number | Optional; defaults to 25 | Power to send channel text. Whole number `0`–`1000`. |
| `message.permissions.channel_picture_power` | number | Optional; defaults to 25 | Power to send channel pictures. Whole number `0`–`1000`. |
| `message.permissions.stream_power` | number | Optional; defaults to 25 | Power to stream channel video. Whole number `0`–`1000`. |
| `message.permissions.move_power` | number | Optional; defaults to 0 | Power to move another client between channels. Whole number `0`–`1000`. |
| `message.permissions.move_protection` | number | Optional; defaults to 0 | Protection against being moved. Whole number `0`–`1000`. |
| `message.permissions.moderation_power` | number | Optional; defaults to 0 | Power to moderate another client. Whole number `0`–`1000`. |
| `message.permissions.moderation_protection` | number | Optional; defaults to 0 | Protection against client moderation. Whole number `0`–`1000`. |
| `message.permissions.permission_edit_power` | number | Optional; defaults to 0 | Power to edit permission definitions. Whole number `0`–`1000`. |
| `message.permissions.grants` | object | Optional; omitted thresholds default to 1000 | Editing thresholds keyed by each supported action or power name; values are whole numbers `0`–`1000`. |
| `message.permissions.grants.create_channel` | number | Optional; defaults to 1000 | Editing threshold for `create_channel`. |
| `message.permissions.grants.create_temp_channel` | number | Optional; defaults to 1000 | Editing threshold for `create_temp_channel`. |
| `message.permissions.grants.kick` | number | Optional; defaults to 1000 | Editing threshold for `kick`. |
| `message.permissions.grants.ban` | number | Optional; defaults to 1000 | Editing threshold for `ban`. |
| `message.permissions.grants.view_stored_users` | number | Optional; defaults to 1000 | Editing threshold for `view_stored_users`. |
| `message.permissions.grants.view_group_permissions` | number | Optional; defaults to 1000 | Editing threshold for `view_group_permissions`. |
| `message.permissions.grants.view_group_members` | number | Optional; defaults to 1000 | Editing threshold for `view_group_members`. |
| `message.permissions.grants.create_groups` | number | Optional; defaults to 1000 | Editing threshold for `create_groups`. |
| `message.permissions.grants.edit_groups` | number | Optional; defaults to 1000 | Editing threshold for `edit_groups`. |
| `message.permissions.grants.delete_groups` | number | Optional; defaults to 1000 | Editing threshold for `delete_groups`. |
| `message.permissions.grants.assign_groups` | number | Optional; defaults to 1000 | Editing threshold for `assign_groups`. |
| `message.permissions.grants.remove_groups` | number | Optional; defaults to 1000 | Editing threshold for `remove_groups`. |
| `message.permissions.grants.edit_group_permissions` | number | Optional; defaults to 1000 | Editing threshold for `edit_group_permissions`. |
| `message.permissions.grants.upload_icons` | number | Optional; defaults to 1000 | Editing threshold for `upload_icons`. |
| `message.permissions.grants.view_client_details` | number | Optional; defaults to 1000 | Editing threshold for `view_client_details`. |
| `message.permissions.grants.view_server_logs` | number | Optional; defaults to 1000 | Editing threshold for `view_server_logs`. |
| `message.permissions.grants.view_server_info` | number | Optional; defaults to 1000 | Editing threshold for `view_server_info`. |
| `message.permissions.grants.send_offline_messages` | number | Optional; defaults to 1000 | Editing threshold for `send_offline_messages`. |
| `message.permissions.grants.receive_offline_messages` | number | Optional; defaults to 1000 | Editing threshold for `receive_offline_messages`. |
| `message.permissions.grants.message_idle_clients` | number | Optional; defaults to 1000 | Editing threshold for `message_idle_clients`. |
| `message.permissions.grants.call_idle_clients` | number | Optional; defaults to 1000 | Editing threshold for `call_idle_clients`. |
| `message.permissions.grants.delete_icons` | number | Optional; defaults to 1000 | Editing threshold for `delete_icons`. |
| `message.permissions.grants.upload_emojis` | number | Optional; defaults to 1000 | Editing threshold for `upload_emojis`. |
| `message.permissions.grants.delete_emojis` | number | Optional; defaults to 1000 | Editing threshold for `delete_emojis`. |
| `message.permissions.grants.view_ip_addresses` | number | Optional; defaults to 1000 | Editing threshold for `view_ip_addresses`. |
| `message.permissions.grants.view_bans` | number | Optional; defaults to 1000 | Editing threshold for `view_bans`. |
| `message.permissions.grants.remove_bans` | number | Optional; defaults to 1000 | Editing threshold for `remove_bans`. |
| `message.permissions.grants.move_clients` | number | Optional; defaults to 1000 | Editing threshold for `move_clients`. |
| `message.permissions.grants.kick_from_channel` | number | Optional; defaults to 1000 | Editing threshold for `kick_from_channel`. |
| `message.permissions.grants.mute_clients` | number | Optional; defaults to 1000 | Editing threshold for `mute_clients`. |
| `message.permissions.grants.stop_client_streams` | number | Optional; defaults to 1000 | Editing threshold for `stop_client_streams`. |
| `message.permissions.grants.edit_channel_name` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_name`. |
| `message.permissions.grants.edit_channel_description` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_description`. |
| `message.permissions.grants.edit_channel_password` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_password`. |
| `message.permissions.grants.edit_channel_powers` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_powers`. |
| `message.permissions.grants.edit_channel_options` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_options`. |
| `message.permissions.grants.delete_channel` | number | Optional; defaults to 1000 | Editing threshold for `delete_channel`. |
| `message.permissions.grants.assign_channel_groups` | number | Optional; defaults to 1000 | Editing threshold for `assign_channel_groups`. |
| `message.permissions.grants.send_direct_pictures` | number | Optional; defaults to 1000 | Editing threshold for `send_direct_pictures`. |
| `message.permissions.grants.send_direct_files` | number | Optional; defaults to 1000 | Editing threshold for `send_direct_files`. |
| `message.permissions.grants.send_channel_pictures` | number | Optional; defaults to 1000 | Editing threshold for `send_channel_pictures`. |
| `message.permissions.grants.send_channel_files` | number | Optional; defaults to 1000 | Editing threshold for `send_channel_files`. |
| `message.permissions.grants.create_music_bots` | number | Optional; defaults to 1000 | Editing threshold for `create_music_bots`. |
| `message.permissions.grants.delete_music_bots` | number | Optional; defaults to 1000 | Editing threshold for `delete_music_bots`. |
| `message.permissions.grants.upload_music_tracks` | number | Optional; defaults to 1000 | Editing threshold for `upload_music_tracks`. |
| `message.permissions.grants.delete_music_tracks` | number | Optional; defaults to 1000 | Editing threshold for `delete_music_tracks`. |
| `message.permissions.grants.control_music_playback` | number | Optional; defaults to 1000 | Editing threshold for `control_music_playback`. |
| `message.permissions.grants.use_reserved_slots` | number | Optional; defaults to 1000 | Editing threshold for `use_reserved_slots`. |
| `message.permissions.grants.create_channel_groups` | number | Optional; defaults to 1000 | Editing threshold for `create_channel_groups`. |
| `message.permissions.grants.edit_channel_groups` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_groups`. |
| `message.permissions.grants.delete_channel_groups` | number | Optional; defaults to 1000 | Editing threshold for `delete_channel_groups`. |
| `message.permissions.grants.view_client_connection_time` | number | Optional; defaults to 1000 | Editing threshold for `view_client_connection_time`. |
| `message.permissions.grants.bypass_channel_password` | number | Optional; defaults to 1000 | Editing threshold for `bypass_channel_password`. |
| `message.permissions.grants.kick_power` | number | Optional; defaults to 1000 | Editing threshold for `kick_power`. |
| `message.permissions.grants.kick_protection` | number | Optional; defaults to 1000 | Editing threshold for `kick_protection`. |
| `message.permissions.grants.ban_power` | number | Optional; defaults to 1000 | Editing threshold for `ban_power`. |
| `message.permissions.grants.ban_protection` | number | Optional; defaults to 1000 | Editing threshold for `ban_protection`. |
| `message.permissions.grants.group_management_power` | number | Optional; defaults to 1000 | Editing threshold for `group_management_power`. |
| `message.permissions.grants.group_protection` | number | Optional; defaults to 1000 | Editing threshold for `group_protection`. |
| `message.permissions.grants.talk_power` | number | Optional; defaults to 1000 | Editing threshold for `talk_power`. |
| `message.permissions.grants.channel_message_power` | number | Optional; defaults to 1000 | Editing threshold for `channel_message_power`. |
| `message.permissions.grants.channel_picture_power` | number | Optional; defaults to 1000 | Editing threshold for `channel_picture_power`. |
| `message.permissions.grants.stream_power` | number | Optional; defaults to 1000 | Editing threshold for `stream_power`. |
| `message.permissions.grants.move_power` | number | Optional; defaults to 1000 | Editing threshold for `move_power`. |
| `message.permissions.grants.move_protection` | number | Optional; defaults to 1000 | Editing threshold for `move_protection`. |
| `message.permissions.grants.moderation_power` | number | Optional; defaults to 1000 | Editing threshold for `moderation_power`. |
| `message.permissions.grants.moderation_protection` | number | Optional; defaults to 1000 | Editing threshold for `moderation_protection`. |
| `message.permissions.grants.permission_edit_power` | number | Optional; defaults to 1000 | Editing threshold for `permission_edit_power`. |
| `message.permissions.flags` | object | Optional | Optional flags keyed by supported action/power names. Only nonzero flag sets are emitted. Unsupported flag names, including `skip` where unavailable, are invalid even when false. |
| `message.permissions.flags.create_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_temp_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_temp_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_temp_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick`. An action needs an explicit non-default decision. |
| `message.permissions.flags.kick.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.ban` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban`. An action needs an explicit non-default decision. |
| `message.permissions.flags.ban.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_stored_users` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_stored_users`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_stored_users.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_group_permissions` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_group_permissions`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_group_permissions.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_group_members` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_group_members`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_group_members.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.assign_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.assign_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.remove_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `remove_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.remove_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_group_permissions` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_group_permissions`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_group_permissions.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.upload_icons` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_icons`. An action needs an explicit non-default decision. |
| `message.permissions.flags.upload_icons.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_client_details` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_client_details`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_client_details.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_server_logs` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_server_logs`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_server_logs.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_server_info` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_server_info`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_server_info.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_offline_messages` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_offline_messages`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_offline_messages.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.receive_offline_messages` | object | Optional; only nonzero flag sets are emitted | Flag object for `receive_offline_messages`. An action needs an explicit non-default decision. |
| `message.permissions.flags.receive_offline_messages.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.message_idle_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `message_idle_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.message_idle_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.call_idle_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `call_idle_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.call_idle_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_icons` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_icons`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_icons.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.upload_emojis` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_emojis`. An action needs an explicit non-default decision. |
| `message.permissions.flags.upload_emojis.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_emojis` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_emojis`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_emojis.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_ip_addresses` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_ip_addresses`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_ip_addresses.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_bans` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_bans`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_bans.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.remove_bans` | object | Optional; only nonzero flag sets are emitted | Flag object for `remove_bans`. An action needs an explicit non-default decision. |
| `message.permissions.flags.remove_bans.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.move_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.move_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick_from_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_from_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.kick_from_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick_from_channel.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.mute_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `mute_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.mute_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.mute_clients.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.stop_client_streams` | object | Optional; only nonzero flag sets are emitted | Flag object for `stop_client_streams`. An action needs an explicit non-default decision. |
| `message.permissions.flags.stop_client_streams.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.stop_client_streams.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_name` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_name`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_name.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_name.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_description` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_description`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_description.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_description.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_password`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_password.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_powers` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_powers`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_powers.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_powers.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_options` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_options`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_options.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_options.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.delete_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_channel.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.assign_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.assign_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.assign_channel_groups.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.send_direct_pictures` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_direct_pictures`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_direct_pictures.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_direct_files` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_direct_files`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_direct_files.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_channel_pictures` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_channel_pictures`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_channel_pictures.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_channel_files` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_channel_files`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_channel_files.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_music_bots` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_music_bots`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_music_bots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_music_bots` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_music_bots`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_music_bots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.upload_music_tracks` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_music_tracks`. An action needs an explicit non-default decision. |
| `message.permissions.flags.upload_music_tracks.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_music_tracks` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_music_tracks`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_music_tracks.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.control_music_playback` | object | Optional; only nonzero flag sets are emitted | Flag object for `control_music_playback`. An action needs an explicit non-default decision. |
| `message.permissions.flags.control_music_playback.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.use_reserved_slots` | object | Optional; only nonzero flag sets are emitted | Flag object for `use_reserved_slots`. An action needs an explicit non-default decision. |
| `message.permissions.flags.use_reserved_slots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_client_connection_time` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_client_connection_time`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_client_connection_time.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.bypass_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `bypass_channel_password`. An action needs an explicit non-default decision. |
| `message.permissions.flags.bypass_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.bypass_channel_password.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.kick_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_power`. Numeric power flags. |
| `message.permissions.flags.kick_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_protection`. Numeric power flags. |
| `message.permissions.flags.kick_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.ban_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban_power`. Numeric power flags. |
| `message.permissions.flags.ban_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.ban_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban_protection`. Numeric power flags. |
| `message.permissions.flags.ban_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.group_management_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `group_management_power`. Numeric power flags. |
| `message.permissions.flags.group_management_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.group_management_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.group_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `group_protection`. Numeric power flags. |
| `message.permissions.flags.group_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.group_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.talk_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `talk_power`. Numeric power flags. |
| `message.permissions.flags.talk_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.talk_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.channel_message_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_message_power`. Numeric power flags. |
| `message.permissions.flags.channel_message_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_message_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.channel_picture_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_picture_power`. Numeric power flags. |
| `message.permissions.flags.channel_picture_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_picture_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.stream_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `stream_power`. Numeric power flags. |
| `message.permissions.flags.stream_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.stream_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.move_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_power`. Numeric power flags. |
| `message.permissions.flags.move_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.move_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.move_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_protection`. Numeric power flags. |
| `message.permissions.flags.move_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.move_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.moderation_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `moderation_power`. Numeric power flags. |
| `message.permissions.flags.moderation_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.moderation_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.moderation_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `moderation_protection`. Numeric power flags. |
| `message.permissions.flags.moderation_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.moderation_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.permission_edit_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `permission_edit_power`. Numeric power flags. |
| `message.permissions.flags.permission_edit_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.manage_icons` | string | Optional legacy input | Legacy input alias for `upload_icons` and `delete_icons` when those members are omitted. Accepts `"grant"`, `"deny"` or `"default"`; never emitted in current output. |

### Client request: `save_groups`

**Body**

```json
{
    "message":
    {
        "type": "save_groups"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"save_groups"`. |

No additional body fields.

### Client request: `server_settings_icon_upload`

**Body**

```json
{
    "message":
    {
        "type": "server_settings_icon_upload",
        "base64_icon_value": "data:image/png;base64,<icon bytes>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_settings_icon_upload"`. |
| `message.base64_icon_value` | string | Required | Base64 icon/data-URL string: at least 64 characters, below `ICON_MAX_LENGTH` (40960), and no longer than `(icon_max_size_bytes * 4 / 3) + 64`. This request uses `base64_icon_value`; responses use `base64_icon`. |

### Client request: `server_settings_delete_icon`

**Body**

```json
{
    "message":
    {
        "type": "server_settings_delete_icon",
        "icon_id": 4
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_settings_delete_icon"`. |
| `message.icon_id` | number | Required | Existing server icon ID. |

### Client request: `server_settings_set_group_icon`

**Body**

```json
{
    "message":
    {
        "type": "server_settings_set_group_icon",
        "group_id": 2,
        "icon_id": 4
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_settings_set_group_icon"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.icon_id` | number | Optional | Existing server icon ID. Omission removes the icon; explicit `null` is invalid. |

**Remove group icon**

```json
{
    "message":
    {
        "type": "server_settings_set_group_icon",
        "group_id": 2
    }
}
```

### Client request: `request_identity_list`

**Body**

```json
{
    "message":
    {
        "type": "request_identity_list"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"request_identity_list"`. |

No additional body fields.

### Client request: `delete_identity`

**Body**

```json
{
    "message":
    {
        "type": "delete_identity",
        "public_key_hash": "<base64 identity hash>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"delete_identity"`. |
| `message.public_key_hash` | string | Required | Stored public-key identity hash. This field is named `public_key_hash`, not `identity_hash`. |

### Client request: `modify_identity_group`

**Body**

```json
{
    "message":
    {
        "type": "modify_identity_group",
        "public_key_hash": "<base64 identity hash>",
        "group_id": 2,
        "add": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"modify_identity_group"`. |
| `message.public_key_hash` | string | Required | Stored public-key identity hash. This field is named `public_key_hash`, not `identity_hash`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.add` | boolean | Required | True adds the group; false removes it. |

### Client request: `get_client_info`

**Body**

```json
{
    "message":
    {
        "type": "get_client_info",
        "client_id": 7,
        "request_id": 41
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"get_client_info"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.request_id` | number | Optional | Optional numeric correlation value; echoed when in the range `0`–`2147483647`. |

### Client request: `admin_log_request`

**Body**

```json
{
    "message":
    {
        "type": "admin_log_request"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"admin_log_request"`. |

No additional body fields.

### Client request: `admin_log_clear`

**Body**

```json
{
    "message":
    {
        "type": "admin_log_clear"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"admin_log_clear"`. |

No additional body fields.

### Client request: `kick`

**Body**

```json
{
    "message":
    {
        "type": "kick",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"kick"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Client request: `ban`

**Body**

```json
{
    "message":
    {
        "type": "ban",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"ban"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Client request: `remove_ban`

**Body**

```json
{
    "message":
    {
        "type": "remove_ban",
        "ip_address": "192.0.2.8"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"remove_ban"`. |
| `message.ip_address` | string | Required | IP address whose matching ban records are all removed together. A failed save leaves those records in effect. |

### Server response: `admin_status`

**Body**

```json
{
    "message":
    {
        "type": "admin_status",
        "is_admin": false
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"admin_status"`. |
| `message.is_admin` | boolean | Required | Whether the receiver currently has administrator status. |

### Server response: `force_admin_password_change`

**Body**

```json
{
    "message":
    {
        "type": "force_admin_password_change"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"force_admin_password_change"`. |

No additional body fields.

### Server response: `access_denied`

**Body**

```json
{
    "message":
    {
        "type": "access_denied",
        "reason": "Insufficient permissions."
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"access_denied"`. |
| `message.reason` | string | Required | Readable denial text. Default: `Insufficient permissions.` This is not a fixed code enum. |

### Server response: `client_info`

**Body**

```json
{
    "message":
    {
        "type": "client_info",
        "client_id": 7,
        "request_id": 41,
        "connected_seconds": 120,
        "last_action_seconds_ago": 4,
        "ip_address": "192.0.2.8",
        "country_iso_code": "SK",
        "identity": "<RSA public key>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_info"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.request_id` | number | Optional | Optional numeric correlation value; echoed when in the range `0`–`2147483647`. |
| `message.connected_seconds` | number | Conditional: view_client_connection_time | Connection duration in seconds. |
| `message.last_action_seconds_ago` | number | Conditional: view_client_details | Seconds since the client's last action. |
| `message.ip_address` | string | Conditional: view_client_details and view_ip_addresses | Client IP address. |
| `message.country_iso_code` | string | Conditional: view_client_details | Two-letter country code; `""` when unknown or hidden. |
| `message.identity` | string | Conditional: view_client_details | Current client RSA public key, not the identity hash. |

**Minimal visible body**

```json
{
    "message":
    {
        "type": "client_info",
        "client_id": 7,
        "request_id": 41
    }
}
```

### Server response: `admin_log`

**Body**

```json
{
    "message":
    {
        "type": "admin_log",
        "lines":
        ["2026-11-01 12:00:00 Example log entry"]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"admin_log"`. |
| `message.lines` | string[] | Required | Log lines in their stored text format. `[]` represents an empty log. |

### Server response: `icon_list`

**Body**

```json
{
    "message":
    {
        "type": "icon_list",
        "icons":
        [
            {
                "icon_id": 4,
                "base64_icon": "<base64 icon>"
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"icon_list"`. |
| `message.icons` | object[] | Required | Icon entries; `[]` means no icons. |
| `message.icons[].icon_id` | number | Required | Server icon ID. |
| `message.icons[].base64_icon` | string | Required | Base64 icon content. The field is named `base64_icon` in responses. |

### Server response: `icon_add`

**Body**

```json
{
    "message":
    {
        "type": "icon_add",
        "icon_id": 4,
        "base64_icon": "<base64 icon>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"icon_add"`. |
| `message.icon_id` | number | Required | Server icon ID. |
| `message.base64_icon` | string | Required | Base64 icon content. The field is named `base64_icon` in responses. |

### Server response: `icon_delete`

**Body**

```json
{
    "message":
    {
        "type": "icon_delete",
        "icon_id": 4
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"icon_delete"`. |
| `message.icon_id` | number | Required | Server icon ID. |

### Server response: `group_list`

The example includes permission definitions. When hidden, each affected entry omits `permissions` entirely; it does not send `null` or an empty replacement object.

**Body**

```json
{
    "message":
    {
        "type": "group_list",
        "groups":
        [
            {
                "group_id": 2,
                "group_name": "Members",
                "group_type": "normal",
                "is_default_guest": false,
                "group_linked_icon_id": 4,
                "has_icon": true,
                "access":
                {
                    "edit_groups": false,
                    "delete_groups": false,
                    "assign_groups": false,
                    "remove_groups": false,
                    "edit_group_permissions": false
                },
                "permissions":
                {
                    "create_channel": "default",
                    "create_temp_channel": "default",
                    "kick": "default",
                    "ban": "default",
                    "view_stored_users": "default",
                    "view_group_permissions": "default",
                    "view_group_members": "default",
                    "create_groups": "default",
                    "edit_groups": "default",
                    "delete_groups": "default",
                    "assign_groups": "default",
                    "remove_groups": "default",
                    "edit_group_permissions": "default",
                    "upload_icons": "default",
                    "view_client_details": "default",
                    "view_server_logs": "default",
                    "view_server_info": "default",
                    "send_offline_messages": "default",
                    "receive_offline_messages": "default",
                    "message_idle_clients": "default",
                    "call_idle_clients": "default",
                    "delete_icons": "default",
                    "upload_emojis": "default",
                    "delete_emojis": "default",
                    "view_ip_addresses": "default",
                    "view_bans": "default",
                    "remove_bans": "default",
                    "move_clients": "default",
                    "kick_from_channel": "default",
                    "mute_clients": "default",
                    "stop_client_streams": "default",
                    "edit_channel_name": "grant",
                    "edit_channel_description": "default",
                    "edit_channel_password": "default",
                    "edit_channel_powers": "default",
                    "edit_channel_options": "default",
                    "delete_channel": "default",
                    "assign_channel_groups": "default",
                    "send_direct_pictures": "default",
                    "send_direct_files": "default",
                    "send_channel_pictures": "default",
                    "send_channel_files": "default",
                    "create_music_bots": "default",
                    "delete_music_bots": "default",
                    "upload_music_tracks": "default",
                    "delete_music_tracks": "default",
                    "control_music_playback": "default",
                    "use_reserved_slots": "default",
                    "create_channel_groups": "default",
                    "edit_channel_groups": "default",
                    "delete_channel_groups": "default",
                    "view_client_connection_time": "default",
                    "bypass_channel_password": "default",
                    "kick_power": 0,
                    "kick_protection": 0,
                    "ban_power": 0,
                    "ban_protection": 0,
                    "group_management_power": 0,
                    "group_protection": 0,
                    "talk_power": 25,
                    "channel_message_power": 25,
                    "channel_picture_power": 25,
                    "stream_power": 25,
                    "move_power": 0,
                    "move_protection": 0,
                    "moderation_power": 0,
                    "moderation_protection": 0,
                    "permission_edit_power": 0,
                    "grants":
                    {
                        "create_channel": 1000,
                        "create_temp_channel": 1000,
                        "kick": 1000,
                        "ban": 1000,
                        "view_stored_users": 1000,
                        "view_group_permissions": 1000,
                        "view_group_members": 1000,
                        "create_groups": 1000,
                        "edit_groups": 1000,
                        "delete_groups": 1000,
                        "assign_groups": 1000,
                        "remove_groups": 1000,
                        "edit_group_permissions": 1000,
                        "upload_icons": 1000,
                        "view_client_details": 1000,
                        "view_server_logs": 1000,
                        "view_server_info": 1000,
                        "send_offline_messages": 1000,
                        "receive_offline_messages": 1000,
                        "message_idle_clients": 1000,
                        "call_idle_clients": 1000,
                        "delete_icons": 1000,
                        "upload_emojis": 1000,
                        "delete_emojis": 1000,
                        "view_ip_addresses": 1000,
                        "view_bans": 1000,
                        "remove_bans": 1000,
                        "move_clients": 1000,
                        "kick_from_channel": 1000,
                        "mute_clients": 1000,
                        "stop_client_streams": 1000,
                        "edit_channel_name": 1000,
                        "edit_channel_description": 1000,
                        "edit_channel_password": 1000,
                        "edit_channel_powers": 1000,
                        "edit_channel_options": 1000,
                        "delete_channel": 1000,
                        "assign_channel_groups": 1000,
                        "send_direct_pictures": 1000,
                        "send_direct_files": 1000,
                        "send_channel_pictures": 1000,
                        "send_channel_files": 1000,
                        "create_music_bots": 1000,
                        "delete_music_bots": 1000,
                        "upload_music_tracks": 1000,
                        "delete_music_tracks": 1000,
                        "control_music_playback": 1000,
                        "use_reserved_slots": 1000,
                        "create_channel_groups": 1000,
                        "edit_channel_groups": 1000,
                        "delete_channel_groups": 1000,
                        "view_client_connection_time": 1000,
                        "bypass_channel_password": 1000,
                        "kick_power": 1000,
                        "kick_protection": 1000,
                        "ban_power": 1000,
                        "ban_protection": 1000,
                        "group_management_power": 1000,
                        "group_protection": 1000,
                        "talk_power": 1000,
                        "channel_message_power": 1000,
                        "channel_picture_power": 1000,
                        "stream_power": 1000,
                        "move_power": 1000,
                        "move_protection": 1000,
                        "moderation_power": 1000,
                        "moderation_protection": 1000,
                        "permission_edit_power": 1000
                    },
                    "flags":
                    {
                        "edit_channel_name":
                        {
                            "negate": true
                        }
                    }
                }
            }
        ],
        "access":
        {
            "create_channel": false,
            "create_temp_channel": false,
            "kick": false,
            "ban": false,
            "view_stored_users": false,
            "view_group_permissions": false,
            "view_group_members": false,
            "create_groups": false,
            "edit_groups": false,
            "delete_groups": false,
            "assign_groups": false,
            "remove_groups": false,
            "edit_group_permissions": false,
            "upload_icons": false,
            "view_client_details": false,
            "view_server_logs": false,
            "view_server_info": false,
            "send_offline_messages": false,
            "receive_offline_messages": false,
            "message_idle_clients": false,
            "call_idle_clients": false,
            "delete_icons": false,
            "upload_emojis": false,
            "delete_emojis": false,
            "view_ip_addresses": false,
            "view_bans": false,
            "remove_bans": false,
            "move_clients": false,
            "kick_from_channel": false,
            "mute_clients": false,
            "stop_client_streams": false,
            "edit_channel_name": false,
            "edit_channel_description": false,
            "edit_channel_password": false,
            "edit_channel_powers": false,
            "edit_channel_options": false,
            "delete_channel": false,
            "assign_channel_groups": false,
            "send_direct_pictures": false,
            "send_direct_files": false,
            "send_channel_pictures": false,
            "send_channel_files": false,
            "create_music_bots": false,
            "delete_music_bots": false,
            "upload_music_tracks": false,
            "delete_music_tracks": false,
            "control_music_playback": false,
            "use_reserved_slots": false,
            "create_channel_groups": false,
            "edit_channel_groups": false,
            "delete_channel_groups": false,
            "view_client_connection_time": false,
            "bypass_channel_password": false,
            "powers":
            {
                "kick_power": 0,
                "kick_protection": 0,
                "ban_power": 0,
                "ban_protection": 0,
                "group_management_power": 0,
                "group_protection": 0,
                "talk_power": 25,
                "channel_message_power": 25,
                "channel_picture_power": 25,
                "stream_power": 25,
                "move_power": 0,
                "move_protection": 0,
                "moderation_power": 0,
                "moderation_protection": 0,
                "permission_edit_power": 0
            },
            "channel_id": 3,
            "channel_group_role": 0,
            "temp_channel_id": null
        },
        "channel_groups":
        [
            {
                "group_id": 2,
                "name": "Speakers",
                "icon_id": -1,
                "permissions":
                {
                    "kick_from_channel": "deny",
                    "mute_clients": "deny",
                    "stop_client_streams": "deny",
                    "edit_channel_description": "deny",
                    "edit_channel_name": "grant",
                    "edit_channel_password": "deny",
                    "edit_channel_powers": "deny",
                    "edit_channel_options": "deny",
                    "delete_channel": "deny",
                    "assign_channel_groups": "deny",
                    "bypass_channel_password": "deny",
                    "channel_group_assignment_power": 0,
                    "channel_group_protection": 0,
                    "talk_power": 0,
                    "channel_message_power": 0,
                    "channel_picture_power": 0,
                    "stream_power": 0,
                    "channel_kick_power": 0,
                    "channel_kick_protection": 0,
                    "channel_moderation_power": 0,
                    "channel_moderation_protection": 0,
                    "grants":
                    {
                        "kick_from_channel": 1000,
                        "mute_clients": 1000,
                        "stop_client_streams": 1000,
                        "edit_channel_description": 1000,
                        "edit_channel_name": 1000,
                        "edit_channel_password": 1000,
                        "edit_channel_powers": 1000,
                        "edit_channel_options": 1000,
                        "delete_channel": 1000,
                        "assign_channel_groups": 1000,
                        "bypass_channel_password": 1000,
                        "channel_group_assignment_power": 1000,
                        "channel_group_protection": 1000,
                        "talk_power": 1000,
                        "channel_message_power": 1000,
                        "channel_picture_power": 1000,
                        "stream_power": 1000,
                        "channel_kick_power": 1000,
                        "channel_kick_protection": 1000,
                        "channel_moderation_power": 1000,
                        "channel_moderation_protection": 1000
                    },
                    "flags":
                    {
                        "edit_channel_name":
                        {
                            "negate": true
                        }
                    }
                }
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"group_list"`. |
| `message.groups` | object[] | Required | Server-group entries. Each entry has its own access object. |
| `message.groups[].group_id` | number | Required | Numeric server-group ID. |
| `message.groups[].group_name` | string | Required | Server-group display name. |
| `message.groups[].group_type` | string | Required | Exactly `"normal"`, `"guest"`, `"moderator"` or `"admin"`. |
| `message.groups[].is_default_guest` | boolean | Required | Whether this is the default guest group. |
| `message.groups[].group_linked_icon_id` | number | Required | Assigned icon ID; meaningful only when `has_icon` is true. |
| `message.groups[].has_icon` | boolean | Required | Whether this group has an assigned icon. |
| `message.groups[].access` | object | Required | Receiver's management rights for this server group. |
| `message.groups[].access.edit_groups` | boolean | Required | Whether the receiver may perform `edit_groups` on this particular group. |
| `message.groups[].access.delete_groups` | boolean | Required | Whether the receiver may perform `delete_groups` on this particular group. |
| `message.groups[].access.assign_groups` | boolean | Required | Whether the receiver may perform `assign_groups` on this particular group. |
| `message.groups[].access.remove_groups` | boolean | Required | Whether the receiver may perform `remove_groups` on this particular group. |
| `message.groups[].access.edit_group_permissions` | boolean | Required | Whether the receiver may perform `edit_group_permissions` on this particular group. |
| `message.groups[].permissions` | object | Conditional: receiver may view group permissions | Stored server-group permissions. |
| `message.groups[].permissions.create_channel` | string | Required | Stored permission for `create_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.create_temp_channel` | string | Required | Stored permission for `create_temp_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.kick` | string | Required | Stored permission for `kick`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.ban` | string | Required | Stored permission for `ban`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_stored_users` | string | Required | Stored permission for `view_stored_users`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_group_permissions` | string | Required | Stored permission for `view_group_permissions`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_group_members` | string | Required | Stored permission for `view_group_members`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.create_groups` | string | Required | Stored permission for `create_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_groups` | string | Required | Stored permission for `edit_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.delete_groups` | string | Required | Stored permission for `delete_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.assign_groups` | string | Required | Stored permission for `assign_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.remove_groups` | string | Required | Stored permission for `remove_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_group_permissions` | string | Required | Stored permission for `edit_group_permissions`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.upload_icons` | string | Required | Stored permission for `upload_icons`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_client_details` | string | Required | Stored permission for `view_client_details`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_server_logs` | string | Required | Stored permission for `view_server_logs`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_server_info` | string | Required | Stored permission for `view_server_info`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.send_offline_messages` | string | Required | Stored permission for `send_offline_messages`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.receive_offline_messages` | string | Required | Stored permission for `receive_offline_messages`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.message_idle_clients` | string | Required | Stored permission for `message_idle_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.call_idle_clients` | string | Required | Stored permission for `call_idle_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.delete_icons` | string | Required | Stored permission for `delete_icons`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.upload_emojis` | string | Required | Stored permission for `upload_emojis`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.delete_emojis` | string | Required | Stored permission for `delete_emojis`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_ip_addresses` | string | Required | Stored permission for `view_ip_addresses`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_bans` | string | Required | Stored permission for `view_bans`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.remove_bans` | string | Required | Stored permission for `remove_bans`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.move_clients` | string | Required | Stored permission for `move_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.kick_from_channel` | string | Required | Stored permission for `kick_from_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.mute_clients` | string | Required | Stored permission for `mute_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.stop_client_streams` | string | Required | Stored permission for `stop_client_streams`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_channel_name` | string | Required | Stored permission for `edit_channel_name`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_channel_description` | string | Required | Stored permission for `edit_channel_description`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_channel_password` | string | Required | Stored permission for `edit_channel_password`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_channel_powers` | string | Required | Stored permission for `edit_channel_powers`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_channel_options` | string | Required | Stored permission for `edit_channel_options`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.delete_channel` | string | Required | Stored permission for `delete_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.assign_channel_groups` | string | Required | Stored permission for `assign_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.send_direct_pictures` | string | Required | Stored permission for `send_direct_pictures`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.send_direct_files` | string | Required | Stored permission for `send_direct_files`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.send_channel_pictures` | string | Required | Stored permission for `send_channel_pictures`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.send_channel_files` | string | Required | Stored permission for `send_channel_files`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.create_music_bots` | string | Required | Stored permission for `create_music_bots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.delete_music_bots` | string | Required | Stored permission for `delete_music_bots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.upload_music_tracks` | string | Required | Stored permission for `upload_music_tracks`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.delete_music_tracks` | string | Required | Stored permission for `delete_music_tracks`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.control_music_playback` | string | Required | Stored permission for `control_music_playback`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.use_reserved_slots` | string | Required | Stored permission for `use_reserved_slots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.create_channel_groups` | string | Required | Stored permission for `create_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.edit_channel_groups` | string | Required | Stored permission for `edit_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.delete_channel_groups` | string | Required | Stored permission for `delete_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.view_client_connection_time` | string | Required | Stored permission for `view_client_connection_time`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.bypass_channel_password` | string | Required | Stored permission for `bypass_channel_password`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.groups[].permissions.kick_power` | number | Required | Power to kick a client from the server. Whole number `0`–`1000`. |
| `message.groups[].permissions.kick_protection` | number | Required | Protection against being kicked from the server. Whole number `0`–`1000`. |
| `message.groups[].permissions.ban_power` | number | Required | Power to ban a client from the server. Whole number `0`–`1000`. |
| `message.groups[].permissions.ban_protection` | number | Required | Protection against a server ban. Whole number `0`–`1000`. |
| `message.groups[].permissions.group_management_power` | number | Required | Power to manage server groups and memberships. Whole number `0`–`1000`. |
| `message.groups[].permissions.group_protection` | number | Required | Protection threshold of the server group. Whole number `0`–`1000`. |
| `message.groups[].permissions.talk_power` | number | Required | Power to transmit channel voice. Whole number `0`–`1000`. |
| `message.groups[].permissions.channel_message_power` | number | Required | Power to send channel text. Whole number `0`–`1000`. |
| `message.groups[].permissions.channel_picture_power` | number | Required | Power to send channel pictures. Whole number `0`–`1000`. |
| `message.groups[].permissions.stream_power` | number | Required | Power to stream channel video. Whole number `0`–`1000`. |
| `message.groups[].permissions.move_power` | number | Required | Power to move another client between channels. Whole number `0`–`1000`. |
| `message.groups[].permissions.move_protection` | number | Required | Protection against being moved. Whole number `0`–`1000`. |
| `message.groups[].permissions.moderation_power` | number | Required | Power to moderate another client. Whole number `0`–`1000`. |
| `message.groups[].permissions.moderation_protection` | number | Required | Protection against client moderation. Whole number `0`–`1000`. |
| `message.groups[].permissions.permission_edit_power` | number | Required | Power to edit permission definitions. Whole number `0`–`1000`. |
| `message.groups[].permissions.grants` | object | Required | Editing thresholds keyed by each supported action or power name; values are whole numbers `0`–`1000`. |
| `message.groups[].permissions.grants.create_channel` | number | Required | Editing threshold for `create_channel`. |
| `message.groups[].permissions.grants.create_temp_channel` | number | Required | Editing threshold for `create_temp_channel`. |
| `message.groups[].permissions.grants.kick` | number | Required | Editing threshold for `kick`. |
| `message.groups[].permissions.grants.ban` | number | Required | Editing threshold for `ban`. |
| `message.groups[].permissions.grants.view_stored_users` | number | Required | Editing threshold for `view_stored_users`. |
| `message.groups[].permissions.grants.view_group_permissions` | number | Required | Editing threshold for `view_group_permissions`. |
| `message.groups[].permissions.grants.view_group_members` | number | Required | Editing threshold for `view_group_members`. |
| `message.groups[].permissions.grants.create_groups` | number | Required | Editing threshold for `create_groups`. |
| `message.groups[].permissions.grants.edit_groups` | number | Required | Editing threshold for `edit_groups`. |
| `message.groups[].permissions.grants.delete_groups` | number | Required | Editing threshold for `delete_groups`. |
| `message.groups[].permissions.grants.assign_groups` | number | Required | Editing threshold for `assign_groups`. |
| `message.groups[].permissions.grants.remove_groups` | number | Required | Editing threshold for `remove_groups`. |
| `message.groups[].permissions.grants.edit_group_permissions` | number | Required | Editing threshold for `edit_group_permissions`. |
| `message.groups[].permissions.grants.upload_icons` | number | Required | Editing threshold for `upload_icons`. |
| `message.groups[].permissions.grants.view_client_details` | number | Required | Editing threshold for `view_client_details`. |
| `message.groups[].permissions.grants.view_server_logs` | number | Required | Editing threshold for `view_server_logs`. |
| `message.groups[].permissions.grants.view_server_info` | number | Required | Editing threshold for `view_server_info`. |
| `message.groups[].permissions.grants.send_offline_messages` | number | Required | Editing threshold for `send_offline_messages`. |
| `message.groups[].permissions.grants.receive_offline_messages` | number | Required | Editing threshold for `receive_offline_messages`. |
| `message.groups[].permissions.grants.message_idle_clients` | number | Required | Editing threshold for `message_idle_clients`. |
| `message.groups[].permissions.grants.call_idle_clients` | number | Required | Editing threshold for `call_idle_clients`. |
| `message.groups[].permissions.grants.delete_icons` | number | Required | Editing threshold for `delete_icons`. |
| `message.groups[].permissions.grants.upload_emojis` | number | Required | Editing threshold for `upload_emojis`. |
| `message.groups[].permissions.grants.delete_emojis` | number | Required | Editing threshold for `delete_emojis`. |
| `message.groups[].permissions.grants.view_ip_addresses` | number | Required | Editing threshold for `view_ip_addresses`. |
| `message.groups[].permissions.grants.view_bans` | number | Required | Editing threshold for `view_bans`. |
| `message.groups[].permissions.grants.remove_bans` | number | Required | Editing threshold for `remove_bans`. |
| `message.groups[].permissions.grants.move_clients` | number | Required | Editing threshold for `move_clients`. |
| `message.groups[].permissions.grants.kick_from_channel` | number | Required | Editing threshold for `kick_from_channel`. |
| `message.groups[].permissions.grants.mute_clients` | number | Required | Editing threshold for `mute_clients`. |
| `message.groups[].permissions.grants.stop_client_streams` | number | Required | Editing threshold for `stop_client_streams`. |
| `message.groups[].permissions.grants.edit_channel_name` | number | Required | Editing threshold for `edit_channel_name`. |
| `message.groups[].permissions.grants.edit_channel_description` | number | Required | Editing threshold for `edit_channel_description`. |
| `message.groups[].permissions.grants.edit_channel_password` | number | Required | Editing threshold for `edit_channel_password`. |
| `message.groups[].permissions.grants.edit_channel_powers` | number | Required | Editing threshold for `edit_channel_powers`. |
| `message.groups[].permissions.grants.edit_channel_options` | number | Required | Editing threshold for `edit_channel_options`. |
| `message.groups[].permissions.grants.delete_channel` | number | Required | Editing threshold for `delete_channel`. |
| `message.groups[].permissions.grants.assign_channel_groups` | number | Required | Editing threshold for `assign_channel_groups`. |
| `message.groups[].permissions.grants.send_direct_pictures` | number | Required | Editing threshold for `send_direct_pictures`. |
| `message.groups[].permissions.grants.send_direct_files` | number | Required | Editing threshold for `send_direct_files`. |
| `message.groups[].permissions.grants.send_channel_pictures` | number | Required | Editing threshold for `send_channel_pictures`. |
| `message.groups[].permissions.grants.send_channel_files` | number | Required | Editing threshold for `send_channel_files`. |
| `message.groups[].permissions.grants.create_music_bots` | number | Required | Editing threshold for `create_music_bots`. |
| `message.groups[].permissions.grants.delete_music_bots` | number | Required | Editing threshold for `delete_music_bots`. |
| `message.groups[].permissions.grants.upload_music_tracks` | number | Required | Editing threshold for `upload_music_tracks`. |
| `message.groups[].permissions.grants.delete_music_tracks` | number | Required | Editing threshold for `delete_music_tracks`. |
| `message.groups[].permissions.grants.control_music_playback` | number | Required | Editing threshold for `control_music_playback`. |
| `message.groups[].permissions.grants.use_reserved_slots` | number | Required | Editing threshold for `use_reserved_slots`. |
| `message.groups[].permissions.grants.create_channel_groups` | number | Required | Editing threshold for `create_channel_groups`. |
| `message.groups[].permissions.grants.edit_channel_groups` | number | Required | Editing threshold for `edit_channel_groups`. |
| `message.groups[].permissions.grants.delete_channel_groups` | number | Required | Editing threshold for `delete_channel_groups`. |
| `message.groups[].permissions.grants.view_client_connection_time` | number | Required | Editing threshold for `view_client_connection_time`. |
| `message.groups[].permissions.grants.bypass_channel_password` | number | Required | Editing threshold for `bypass_channel_password`. |
| `message.groups[].permissions.grants.kick_power` | number | Required | Editing threshold for `kick_power`. |
| `message.groups[].permissions.grants.kick_protection` | number | Required | Editing threshold for `kick_protection`. |
| `message.groups[].permissions.grants.ban_power` | number | Required | Editing threshold for `ban_power`. |
| `message.groups[].permissions.grants.ban_protection` | number | Required | Editing threshold for `ban_protection`. |
| `message.groups[].permissions.grants.group_management_power` | number | Required | Editing threshold for `group_management_power`. |
| `message.groups[].permissions.grants.group_protection` | number | Required | Editing threshold for `group_protection`. |
| `message.groups[].permissions.grants.talk_power` | number | Required | Editing threshold for `talk_power`. |
| `message.groups[].permissions.grants.channel_message_power` | number | Required | Editing threshold for `channel_message_power`. |
| `message.groups[].permissions.grants.channel_picture_power` | number | Required | Editing threshold for `channel_picture_power`. |
| `message.groups[].permissions.grants.stream_power` | number | Required | Editing threshold for `stream_power`. |
| `message.groups[].permissions.grants.move_power` | number | Required | Editing threshold for `move_power`. |
| `message.groups[].permissions.grants.move_protection` | number | Required | Editing threshold for `move_protection`. |
| `message.groups[].permissions.grants.moderation_power` | number | Required | Editing threshold for `moderation_power`. |
| `message.groups[].permissions.grants.moderation_protection` | number | Required | Editing threshold for `moderation_protection`. |
| `message.groups[].permissions.grants.permission_edit_power` | number | Required | Editing threshold for `permission_edit_power`. |
| `message.groups[].permissions.flags` | object | Optional | Optional flags keyed by supported action/power names. Only nonzero flag sets are emitted. Unsupported flag names, including `skip` where unavailable, are invalid even when false. |
| `message.groups[].permissions.flags.create_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_channel`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.create_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.create_temp_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_temp_channel`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.create_temp_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.kick` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.kick.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.ban` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.ban.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_stored_users` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_stored_users`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_stored_users.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_group_permissions` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_group_permissions`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_group_permissions.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_group_members` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_group_members`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_group_members.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.create_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.create_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.delete_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.delete_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.assign_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.assign_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.remove_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `remove_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.remove_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_group_permissions` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_group_permissions`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_group_permissions.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.upload_icons` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_icons`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.upload_icons.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_client_details` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_client_details`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_client_details.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_server_logs` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_server_logs`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_server_logs.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_server_info` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_server_info`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_server_info.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.send_offline_messages` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_offline_messages`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.send_offline_messages.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.receive_offline_messages` | object | Optional; only nonzero flag sets are emitted | Flag object for `receive_offline_messages`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.receive_offline_messages.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.message_idle_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `message_idle_clients`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.message_idle_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.call_idle_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `call_idle_clients`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.call_idle_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.delete_icons` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_icons`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.delete_icons.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.upload_emojis` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_emojis`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.upload_emojis.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.delete_emojis` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_emojis`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.delete_emojis.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_ip_addresses` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_ip_addresses`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_ip_addresses.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_bans` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_bans`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_bans.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.remove_bans` | object | Optional; only nonzero flag sets are emitted | Flag object for `remove_bans`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.remove_bans.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.move_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_clients`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.move_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.kick_from_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_from_channel`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.kick_from_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.kick_from_channel.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.mute_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `mute_clients`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.mute_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.mute_clients.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.stop_client_streams` | object | Optional; only nonzero flag sets are emitted | Flag object for `stop_client_streams`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.stop_client_streams.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.stop_client_streams.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.edit_channel_name` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_name`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_channel_name.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_channel_name.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.edit_channel_description` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_description`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_channel_description.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_channel_description.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.edit_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_password`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_channel_password.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.edit_channel_powers` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_powers`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_channel_powers.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_channel_powers.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.edit_channel_options` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_options`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_channel_options.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_channel_options.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.delete_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.delete_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.delete_channel.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.assign_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_channel_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.assign_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.assign_channel_groups.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.send_direct_pictures` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_direct_pictures`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.send_direct_pictures.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.send_direct_files` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_direct_files`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.send_direct_files.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.send_channel_pictures` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_channel_pictures`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.send_channel_pictures.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.send_channel_files` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_channel_files`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.send_channel_files.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.create_music_bots` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_music_bots`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.create_music_bots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.delete_music_bots` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_music_bots`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.delete_music_bots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.upload_music_tracks` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_music_tracks`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.upload_music_tracks.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.delete_music_tracks` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_music_tracks`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.delete_music_tracks.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.control_music_playback` | object | Optional; only nonzero flag sets are emitted | Flag object for `control_music_playback`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.control_music_playback.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.use_reserved_slots` | object | Optional; only nonzero flag sets are emitted | Flag object for `use_reserved_slots`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.use_reserved_slots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.create_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_channel_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.create_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.edit_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.edit_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.delete_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel_groups`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.delete_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.view_client_connection_time` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_client_connection_time`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.view_client_connection_time.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.bypass_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `bypass_channel_password`. An action needs an explicit non-default decision. |
| `message.groups[].permissions.flags.bypass_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.bypass_channel_password.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.kick_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_power`. Numeric power flags. |
| `message.groups[].permissions.flags.kick_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.kick_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_protection`. Numeric power flags. |
| `message.groups[].permissions.flags.kick_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.ban_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban_power`. Numeric power flags. |
| `message.groups[].permissions.flags.ban_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.ban_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban_protection`. Numeric power flags. |
| `message.groups[].permissions.flags.ban_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.group_management_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `group_management_power`. Numeric power flags. |
| `message.groups[].permissions.flags.group_management_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.group_management_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.group_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `group_protection`. Numeric power flags. |
| `message.groups[].permissions.flags.group_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.group_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.talk_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `talk_power`. Numeric power flags. |
| `message.groups[].permissions.flags.talk_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.talk_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.channel_message_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_message_power`. Numeric power flags. |
| `message.groups[].permissions.flags.channel_message_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.channel_message_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.channel_picture_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_picture_power`. Numeric power flags. |
| `message.groups[].permissions.flags.channel_picture_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.channel_picture_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.stream_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `stream_power`. Numeric power flags. |
| `message.groups[].permissions.flags.stream_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.stream_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.move_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_power`. Numeric power flags. |
| `message.groups[].permissions.flags.move_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.move_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.move_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_protection`. Numeric power flags. |
| `message.groups[].permissions.flags.move_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.move_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.moderation_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `moderation_power`. Numeric power flags. |
| `message.groups[].permissions.flags.moderation_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.moderation_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.moderation_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `moderation_protection`. Numeric power flags. |
| `message.groups[].permissions.flags.moderation_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.groups[].permissions.flags.moderation_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.groups[].permissions.flags.permission_edit_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `permission_edit_power`. Numeric power flags. |
| `message.groups[].permissions.flags.permission_edit_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.access` | object | Required | Receiver's effective access and current channel context. |
| `message.access.create_channel` | boolean | Required | Receiver's effective `create_channel` permission; a boolean, not a stored decision string. |
| `message.access.create_temp_channel` | boolean | Required | Receiver's effective `create_temp_channel` permission; a boolean, not a stored decision string. |
| `message.access.kick` | boolean | Required | Receiver's effective `kick` permission; a boolean, not a stored decision string. |
| `message.access.ban` | boolean | Required | Receiver's effective `ban` permission; a boolean, not a stored decision string. |
| `message.access.view_stored_users` | boolean | Required | Receiver's effective `view_stored_users` permission; a boolean, not a stored decision string. |
| `message.access.view_group_permissions` | boolean | Required | Receiver's effective `view_group_permissions` permission; a boolean, not a stored decision string. |
| `message.access.view_group_members` | boolean | Required | Receiver's effective `view_group_members` permission; a boolean, not a stored decision string. |
| `message.access.create_groups` | boolean | Required | Receiver's effective `create_groups` permission; a boolean, not a stored decision string. |
| `message.access.edit_groups` | boolean | Required | Receiver's effective `edit_groups` permission; a boolean, not a stored decision string. |
| `message.access.delete_groups` | boolean | Required | Receiver's effective `delete_groups` permission; a boolean, not a stored decision string. |
| `message.access.assign_groups` | boolean | Required | Receiver's effective `assign_groups` permission; a boolean, not a stored decision string. |
| `message.access.remove_groups` | boolean | Required | Receiver's effective `remove_groups` permission; a boolean, not a stored decision string. |
| `message.access.edit_group_permissions` | boolean | Required | Receiver's effective `edit_group_permissions` permission; a boolean, not a stored decision string. |
| `message.access.upload_icons` | boolean | Required | Receiver's effective `upload_icons` permission; a boolean, not a stored decision string. |
| `message.access.view_client_details` | boolean | Required | Receiver's effective `view_client_details` permission; a boolean, not a stored decision string. |
| `message.access.view_server_logs` | boolean | Required | Receiver's effective `view_server_logs` permission; a boolean, not a stored decision string. |
| `message.access.view_server_info` | boolean | Required | Receiver's effective `view_server_info` permission; a boolean, not a stored decision string. |
| `message.access.send_offline_messages` | boolean | Required | Receiver's effective `send_offline_messages` permission; a boolean, not a stored decision string. |
| `message.access.receive_offline_messages` | boolean | Required | Receiver's effective `receive_offline_messages` permission; a boolean, not a stored decision string. |
| `message.access.message_idle_clients` | boolean | Required | Receiver's effective `message_idle_clients` permission; a boolean, not a stored decision string. |
| `message.access.call_idle_clients` | boolean | Required | Receiver's effective `call_idle_clients` permission; a boolean, not a stored decision string. |
| `message.access.delete_icons` | boolean | Required | Receiver's effective `delete_icons` permission; a boolean, not a stored decision string. |
| `message.access.upload_emojis` | boolean | Required | Receiver's effective `upload_emojis` permission; a boolean, not a stored decision string. |
| `message.access.delete_emojis` | boolean | Required | Receiver's effective `delete_emojis` permission; a boolean, not a stored decision string. |
| `message.access.view_ip_addresses` | boolean | Required | Receiver's effective `view_ip_addresses` permission; a boolean, not a stored decision string. |
| `message.access.view_bans` | boolean | Required | Receiver's effective `view_bans` permission; a boolean, not a stored decision string. |
| `message.access.remove_bans` | boolean | Required | Receiver's effective `remove_bans` permission; a boolean, not a stored decision string. |
| `message.access.move_clients` | boolean | Required | Receiver's effective `move_clients` permission; a boolean, not a stored decision string. |
| `message.access.kick_from_channel` | boolean | Required | Receiver's effective `kick_from_channel` permission; a boolean, not a stored decision string. |
| `message.access.mute_clients` | boolean | Required | Receiver's effective `mute_clients` permission; a boolean, not a stored decision string. |
| `message.access.stop_client_streams` | boolean | Required | Receiver's effective `stop_client_streams` permission; a boolean, not a stored decision string. |
| `message.access.edit_channel_name` | boolean | Required | Receiver's effective `edit_channel_name` permission; a boolean, not a stored decision string. |
| `message.access.edit_channel_description` | boolean | Required | Receiver's effective `edit_channel_description` permission; a boolean, not a stored decision string. |
| `message.access.edit_channel_password` | boolean | Required | Receiver's effective `edit_channel_password` permission; a boolean, not a stored decision string. |
| `message.access.edit_channel_powers` | boolean | Required | Receiver's effective `edit_channel_powers` permission; a boolean, not a stored decision string. |
| `message.access.edit_channel_options` | boolean | Required | Receiver's effective `edit_channel_options` permission; a boolean, not a stored decision string. |
| `message.access.delete_channel` | boolean | Required | Receiver's effective `delete_channel` permission; a boolean, not a stored decision string. |
| `message.access.assign_channel_groups` | boolean | Required | Receiver's effective `assign_channel_groups` permission; a boolean, not a stored decision string. |
| `message.access.send_direct_pictures` | boolean | Required | Receiver's effective `send_direct_pictures` permission; a boolean, not a stored decision string. |
| `message.access.send_direct_files` | boolean | Required | Receiver's effective `send_direct_files` permission; a boolean, not a stored decision string. |
| `message.access.send_channel_pictures` | boolean | Required | Receiver's effective `send_channel_pictures` permission; a boolean, not a stored decision string. |
| `message.access.send_channel_files` | boolean | Required | Receiver's effective `send_channel_files` permission; a boolean, not a stored decision string. |
| `message.access.create_music_bots` | boolean | Required | Receiver's effective `create_music_bots` permission; a boolean, not a stored decision string. |
| `message.access.delete_music_bots` | boolean | Required | Receiver's effective `delete_music_bots` permission; a boolean, not a stored decision string. |
| `message.access.upload_music_tracks` | boolean | Required | Receiver's effective `upload_music_tracks` permission; a boolean, not a stored decision string. |
| `message.access.delete_music_tracks` | boolean | Required | Receiver's effective `delete_music_tracks` permission; a boolean, not a stored decision string. |
| `message.access.control_music_playback` | boolean | Required | Receiver's effective `control_music_playback` permission; a boolean, not a stored decision string. |
| `message.access.use_reserved_slots` | boolean | Required | Receiver's effective `use_reserved_slots` permission; a boolean, not a stored decision string. |
| `message.access.create_channel_groups` | boolean | Required | Receiver's effective `create_channel_groups` permission; a boolean, not a stored decision string. |
| `message.access.edit_channel_groups` | boolean | Required | Receiver's effective `edit_channel_groups` permission; a boolean, not a stored decision string. |
| `message.access.delete_channel_groups` | boolean | Required | Receiver's effective `delete_channel_groups` permission; a boolean, not a stored decision string. |
| `message.access.view_client_connection_time` | boolean | Required | Receiver's effective `view_client_connection_time` permission; a boolean, not a stored decision string. |
| `message.access.bypass_channel_password` | boolean | Required | Receiver's effective `bypass_channel_password` permission; a boolean, not a stored decision string. |
| `message.access.powers` | object | Required | Receiver's effective numeric server powers. The four content powers reflect the current channel. |
| `message.access.powers.kick_power` | number | Required | Power to kick a client from the server. Effective value from `0` through `1000`. |
| `message.access.powers.kick_protection` | number | Required | Protection against being kicked from the server. Effective value from `0` through `1000`. |
| `message.access.powers.ban_power` | number | Required | Power to ban a client from the server. Effective value from `0` through `1000`. |
| `message.access.powers.ban_protection` | number | Required | Protection against a server ban. Effective value from `0` through `1000`. |
| `message.access.powers.group_management_power` | number | Required | Power to manage server groups and memberships. Effective value from `0` through `1000`. |
| `message.access.powers.group_protection` | number | Required | Protection threshold of the server group. Effective value from `0` through `1000`. |
| `message.access.powers.talk_power` | number | Required | Power to transmit channel voice. Effective value from `0` through `1000`. |
| `message.access.powers.channel_message_power` | number | Required | Power to send channel text. Effective value from `0` through `1000`. |
| `message.access.powers.channel_picture_power` | number | Required | Power to send channel pictures. Effective value from `0` through `1000`. |
| `message.access.powers.stream_power` | number | Required | Power to stream channel video. Effective value from `0` through `1000`. |
| `message.access.powers.move_power` | number | Required | Power to move another client between channels. Effective value from `0` through `1000`. |
| `message.access.powers.move_protection` | number | Required | Protection against being moved. Effective value from `0` through `1000`. |
| `message.access.powers.moderation_power` | number | Required | Power to moderate another client. Effective value from `0` through `1000`. |
| `message.access.powers.moderation_protection` | number | Required | Protection against client moderation. Effective value from `0` through `1000`. |
| `message.access.powers.permission_edit_power` | number | Required | Power to edit permission definitions. Effective value from `0` through `1000`. |
| `message.access.channel_id` | number | Required | Numeric channel ID. |
| `message.access.channel_group_role` | number | Required | Receiver's current channel role ID, `0`–`127`. |
| `message.access.temp_channel_id` | number or null | Required | ID of the receiver's temporary channel, or `null` when none exists. |
| `message.channel_groups` | object[] | Required | Defined channel-group entries. |
| `message.channel_groups[].group_id` | number | Required | Defined channel-group ID, `1`–`127`. `0` is the implicit ordinary member role. |
| `message.channel_groups[].name` | string | Required | Channel-group display name. |
| `message.channel_groups[].icon_id` | number | Required | `-2` for the built-in Channel Admin icon, `-1` for no icon, otherwise a server icon ID. |
| `message.channel_groups[].permissions` | object | Conditional: receiver may view group permissions | Stored channel-group permissions; action values are grant/deny. |
| `message.channel_groups[].permissions.kick_from_channel` | string | Required | Stored permission for `kick_from_channel`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.mute_clients` | string | Required | Stored permission for `mute_clients`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.stop_client_streams` | string | Required | Stored permission for `stop_client_streams`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.edit_channel_description` | string | Required | Stored permission for `edit_channel_description`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.edit_channel_name` | string | Required | Stored permission for `edit_channel_name`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.edit_channel_password` | string | Required | Stored permission for `edit_channel_password`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.edit_channel_powers` | string | Required | Stored permission for `edit_channel_powers`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.edit_channel_options` | string | Required | Stored permission for `edit_channel_options`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.delete_channel` | string | Required | Stored permission for `delete_channel`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.assign_channel_groups` | string | Required | Stored permission for `assign_channel_groups`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.bypass_channel_password` | string | Required | Stored permission for `bypass_channel_password`. Output is `"grant"` or `"deny"`. |
| `message.channel_groups[].permissions.channel_group_assignment_power` | number | Required | Power to assign a channel group. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.channel_group_protection` | number | Required | Protection threshold of the channel group. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.talk_power` | number | Required | Power to transmit channel voice. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.channel_message_power` | number | Required | Power to send channel text. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.channel_picture_power` | number | Required | Power to send channel pictures. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.stream_power` | number | Required | Power to stream channel video. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.channel_kick_power` | number | Required | Power to kick a client from a channel. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.channel_kick_protection` | number | Required | Protection against a channel kick. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.channel_moderation_power` | number | Required | Power to mute a client or stop a stream in a channel. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.channel_moderation_protection` | number | Required | Protection against channel moderation. Whole number `0`–`1000`. |
| `message.channel_groups[].permissions.grants` | object | Required | Editing thresholds keyed by each supported action or power name; values are whole numbers `0`–`1000`. |
| `message.channel_groups[].permissions.grants.kick_from_channel` | number | Required | Editing threshold for `kick_from_channel`. |
| `message.channel_groups[].permissions.grants.mute_clients` | number | Required | Editing threshold for `mute_clients`. |
| `message.channel_groups[].permissions.grants.stop_client_streams` | number | Required | Editing threshold for `stop_client_streams`. |
| `message.channel_groups[].permissions.grants.edit_channel_description` | number | Required | Editing threshold for `edit_channel_description`. |
| `message.channel_groups[].permissions.grants.edit_channel_name` | number | Required | Editing threshold for `edit_channel_name`. |
| `message.channel_groups[].permissions.grants.edit_channel_password` | number | Required | Editing threshold for `edit_channel_password`. |
| `message.channel_groups[].permissions.grants.edit_channel_powers` | number | Required | Editing threshold for `edit_channel_powers`. |
| `message.channel_groups[].permissions.grants.edit_channel_options` | number | Required | Editing threshold for `edit_channel_options`. |
| `message.channel_groups[].permissions.grants.delete_channel` | number | Required | Editing threshold for `delete_channel`. |
| `message.channel_groups[].permissions.grants.assign_channel_groups` | number | Required | Editing threshold for `assign_channel_groups`. |
| `message.channel_groups[].permissions.grants.bypass_channel_password` | number | Required | Editing threshold for `bypass_channel_password`. |
| `message.channel_groups[].permissions.grants.channel_group_assignment_power` | number | Required | Editing threshold for `channel_group_assignment_power`. |
| `message.channel_groups[].permissions.grants.channel_group_protection` | number | Required | Editing threshold for `channel_group_protection`. |
| `message.channel_groups[].permissions.grants.talk_power` | number | Required | Editing threshold for `talk_power`. |
| `message.channel_groups[].permissions.grants.channel_message_power` | number | Required | Editing threshold for `channel_message_power`. |
| `message.channel_groups[].permissions.grants.channel_picture_power` | number | Required | Editing threshold for `channel_picture_power`. |
| `message.channel_groups[].permissions.grants.stream_power` | number | Required | Editing threshold for `stream_power`. |
| `message.channel_groups[].permissions.grants.channel_kick_power` | number | Required | Editing threshold for `channel_kick_power`. |
| `message.channel_groups[].permissions.grants.channel_kick_protection` | number | Required | Editing threshold for `channel_kick_protection`. |
| `message.channel_groups[].permissions.grants.channel_moderation_power` | number | Required | Editing threshold for `channel_moderation_power`. |
| `message.channel_groups[].permissions.grants.channel_moderation_protection` | number | Required | Editing threshold for `channel_moderation_protection`. |
| `message.channel_groups[].permissions.flags` | object | Optional | Optional flags keyed by supported action/power names. Only nonzero flag sets are emitted. Unsupported flag names, including `skip` where unavailable, are invalid even when false. |
| `message.channel_groups[].permissions.flags.kick_from_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_from_channel`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.kick_from_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.mute_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `mute_clients`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.mute_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.stop_client_streams` | object | Optional; only nonzero flag sets are emitted | Flag object for `stop_client_streams`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.stop_client_streams.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.edit_channel_description` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_description`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.edit_channel_description.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.edit_channel_name` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_name`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.edit_channel_name.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.edit_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_password`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.edit_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.edit_channel_powers` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_powers`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.edit_channel_powers.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.edit_channel_options` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_options`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.edit_channel_options.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.delete_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.delete_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.assign_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_channel_groups`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.assign_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.bypass_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `bypass_channel_password`. An action needs an explicit non-default decision. |
| `message.channel_groups[].permissions.flags.bypass_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_group_assignment_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_group_assignment_power`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_group_assignment_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_group_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_group_protection`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_group_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.talk_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `talk_power`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.talk_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_message_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_message_power`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_message_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_picture_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_picture_power`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_picture_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.stream_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `stream_power`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.stream_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_kick_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_kick_power`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_kick_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_kick_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_kick_protection`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_kick_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_moderation_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_moderation_power`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_moderation_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.channel_groups[].permissions.flags.channel_moderation_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_moderation_protection`. Numeric power flags. |
| `message.channel_groups[].permissions.flags.channel_moderation_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |

### Server response: `group_add`

**Body**

```json
{
    "message":
    {
        "type": "group_add",
        "group_id": 2,
        "group_name": "Members",
        "group_type": "normal",
        "is_default_guest": false,
        "group_linked_icon_id": 4,
        "has_icon": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"group_add"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.group_name` | string | Required | Server-group display name. |
| `message.group_type` | string | Required | Exactly `"normal"`, `"guest"`, `"moderator"` or `"admin"`. |
| `message.is_default_guest` | boolean | Required | Whether this is the default guest group. |
| `message.group_linked_icon_id` | number | Required | Assigned icon ID; meaningful only when `has_icon` is true. |
| `message.has_icon` | boolean | Required | Whether this group has an assigned icon. |

### Server response: `group_delete`

**Body**

```json
{
    "message":
    {
        "type": "group_delete",
        "group_id": 2
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"group_delete"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |

### Server response: `group_add_to_client`

**Body**

```json
{
    "message":
    {
        "type": "group_add_to_client",
        "group_id": 2,
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"group_add_to_client"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Server response: `remove_group_from_client`

**Body**

```json
{
    "message":
    {
        "type": "remove_group_from_client",
        "group_id": 2,
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"remove_group_from_client"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Server response: `group_icon_changed`

**Body**

```json
{
    "message":
    {
        "type": "group_icon_changed",
        "group_id": 2,
        "has_icon": true,
        "group_linked_icon_id": 4
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"group_icon_changed"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.has_icon` | boolean | Required | Whether an icon is assigned. |
| `message.group_linked_icon_id` | number | Required | Icon ID; meaningful only when `has_icon` is true. |

### Server response: `group_permissions_changed`

**Body**

```json
{
    "message":
    {
        "type": "group_permissions_changed",
        "group_id": 2,
        "permissions":
        {
            "create_channel": "default",
            "create_temp_channel": "default",
            "kick": "default",
            "ban": "default",
            "view_stored_users": "default",
            "view_group_permissions": "default",
            "view_group_members": "default",
            "create_groups": "default",
            "edit_groups": "default",
            "delete_groups": "default",
            "assign_groups": "default",
            "remove_groups": "default",
            "edit_group_permissions": "default",
            "upload_icons": "default",
            "view_client_details": "default",
            "view_server_logs": "default",
            "view_server_info": "default",
            "send_offline_messages": "default",
            "receive_offline_messages": "default",
            "message_idle_clients": "default",
            "call_idle_clients": "default",
            "delete_icons": "default",
            "upload_emojis": "default",
            "delete_emojis": "default",
            "view_ip_addresses": "default",
            "view_bans": "default",
            "remove_bans": "default",
            "move_clients": "default",
            "kick_from_channel": "default",
            "mute_clients": "default",
            "stop_client_streams": "default",
            "edit_channel_name": "grant",
            "edit_channel_description": "default",
            "edit_channel_password": "default",
            "edit_channel_powers": "default",
            "edit_channel_options": "default",
            "delete_channel": "default",
            "assign_channel_groups": "default",
            "send_direct_pictures": "default",
            "send_direct_files": "default",
            "send_channel_pictures": "default",
            "send_channel_files": "default",
            "create_music_bots": "default",
            "delete_music_bots": "default",
            "upload_music_tracks": "default",
            "delete_music_tracks": "default",
            "control_music_playback": "default",
            "use_reserved_slots": "default",
            "create_channel_groups": "default",
            "edit_channel_groups": "default",
            "delete_channel_groups": "default",
            "view_client_connection_time": "default",
            "bypass_channel_password": "default",
            "kick_power": 0,
            "kick_protection": 0,
            "ban_power": 0,
            "ban_protection": 0,
            "group_management_power": 0,
            "group_protection": 0,
            "talk_power": 25,
            "channel_message_power": 25,
            "channel_picture_power": 25,
            "stream_power": 25,
            "move_power": 0,
            "move_protection": 0,
            "moderation_power": 0,
            "moderation_protection": 0,
            "permission_edit_power": 0,
            "grants":
            {
                "create_channel": 1000,
                "create_temp_channel": 1000,
                "kick": 1000,
                "ban": 1000,
                "view_stored_users": 1000,
                "view_group_permissions": 1000,
                "view_group_members": 1000,
                "create_groups": 1000,
                "edit_groups": 1000,
                "delete_groups": 1000,
                "assign_groups": 1000,
                "remove_groups": 1000,
                "edit_group_permissions": 1000,
                "upload_icons": 1000,
                "view_client_details": 1000,
                "view_server_logs": 1000,
                "view_server_info": 1000,
                "send_offline_messages": 1000,
                "receive_offline_messages": 1000,
                "message_idle_clients": 1000,
                "call_idle_clients": 1000,
                "delete_icons": 1000,
                "upload_emojis": 1000,
                "delete_emojis": 1000,
                "view_ip_addresses": 1000,
                "view_bans": 1000,
                "remove_bans": 1000,
                "move_clients": 1000,
                "kick_from_channel": 1000,
                "mute_clients": 1000,
                "stop_client_streams": 1000,
                "edit_channel_name": 1000,
                "edit_channel_description": 1000,
                "edit_channel_password": 1000,
                "edit_channel_powers": 1000,
                "edit_channel_options": 1000,
                "delete_channel": 1000,
                "assign_channel_groups": 1000,
                "send_direct_pictures": 1000,
                "send_direct_files": 1000,
                "send_channel_pictures": 1000,
                "send_channel_files": 1000,
                "create_music_bots": 1000,
                "delete_music_bots": 1000,
                "upload_music_tracks": 1000,
                "delete_music_tracks": 1000,
                "control_music_playback": 1000,
                "use_reserved_slots": 1000,
                "create_channel_groups": 1000,
                "edit_channel_groups": 1000,
                "delete_channel_groups": 1000,
                "view_client_connection_time": 1000,
                "bypass_channel_password": 1000,
                "kick_power": 1000,
                "kick_protection": 1000,
                "ban_power": 1000,
                "ban_protection": 1000,
                "group_management_power": 1000,
                "group_protection": 1000,
                "talk_power": 1000,
                "channel_message_power": 1000,
                "channel_picture_power": 1000,
                "stream_power": 1000,
                "move_power": 1000,
                "move_protection": 1000,
                "moderation_power": 1000,
                "moderation_protection": 1000,
                "permission_edit_power": 1000
            },
            "flags":
            {
                "edit_channel_name":
                {
                    "negate": true
                }
            }
        }
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"group_permissions_changed"`. |
| `message.group_id` | number | Required | Numeric server-group ID. |
| `message.permissions` | object | Required | Complete stored server-group permission definition. |
| `message.permissions.create_channel` | string | Required | Stored permission for `create_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_temp_channel` | string | Required | Stored permission for `create_temp_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.kick` | string | Required | Stored permission for `kick`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.ban` | string | Required | Stored permission for `ban`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_stored_users` | string | Required | Stored permission for `view_stored_users`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_group_permissions` | string | Required | Stored permission for `view_group_permissions`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_group_members` | string | Required | Stored permission for `view_group_members`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_groups` | string | Required | Stored permission for `create_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_groups` | string | Required | Stored permission for `edit_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_groups` | string | Required | Stored permission for `delete_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.assign_groups` | string | Required | Stored permission for `assign_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.remove_groups` | string | Required | Stored permission for `remove_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_group_permissions` | string | Required | Stored permission for `edit_group_permissions`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.upload_icons` | string | Required | Stored permission for `upload_icons`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_client_details` | string | Required | Stored permission for `view_client_details`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_server_logs` | string | Required | Stored permission for `view_server_logs`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_server_info` | string | Required | Stored permission for `view_server_info`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_offline_messages` | string | Required | Stored permission for `send_offline_messages`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.receive_offline_messages` | string | Required | Stored permission for `receive_offline_messages`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.message_idle_clients` | string | Required | Stored permission for `message_idle_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.call_idle_clients` | string | Required | Stored permission for `call_idle_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_icons` | string | Required | Stored permission for `delete_icons`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.upload_emojis` | string | Required | Stored permission for `upload_emojis`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_emojis` | string | Required | Stored permission for `delete_emojis`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_ip_addresses` | string | Required | Stored permission for `view_ip_addresses`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_bans` | string | Required | Stored permission for `view_bans`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.remove_bans` | string | Required | Stored permission for `remove_bans`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.move_clients` | string | Required | Stored permission for `move_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.kick_from_channel` | string | Required | Stored permission for `kick_from_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.mute_clients` | string | Required | Stored permission for `mute_clients`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.stop_client_streams` | string | Required | Stored permission for `stop_client_streams`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_name` | string | Required | Stored permission for `edit_channel_name`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_description` | string | Required | Stored permission for `edit_channel_description`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_password` | string | Required | Stored permission for `edit_channel_password`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_powers` | string | Required | Stored permission for `edit_channel_powers`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_options` | string | Required | Stored permission for `edit_channel_options`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_channel` | string | Required | Stored permission for `delete_channel`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.assign_channel_groups` | string | Required | Stored permission for `assign_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_direct_pictures` | string | Required | Stored permission for `send_direct_pictures`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_direct_files` | string | Required | Stored permission for `send_direct_files`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_channel_pictures` | string | Required | Stored permission for `send_channel_pictures`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.send_channel_files` | string | Required | Stored permission for `send_channel_files`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_music_bots` | string | Required | Stored permission for `create_music_bots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_music_bots` | string | Required | Stored permission for `delete_music_bots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.upload_music_tracks` | string | Required | Stored permission for `upload_music_tracks`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_music_tracks` | string | Required | Stored permission for `delete_music_tracks`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.control_music_playback` | string | Required | Stored permission for `control_music_playback`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.use_reserved_slots` | string | Required | Stored permission for `use_reserved_slots`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.create_channel_groups` | string | Required | Stored permission for `create_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.edit_channel_groups` | string | Required | Stored permission for `edit_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.delete_channel_groups` | string | Required | Stored permission for `delete_channel_groups`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.view_client_connection_time` | string | Required | Stored permission for `view_client_connection_time`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.bypass_channel_password` | string | Required | Stored permission for `bypass_channel_password`. Exactly `"grant"`, `"deny"` or `"default"`. |
| `message.permissions.kick_power` | number | Required | Power to kick a client from the server. Whole number `0`–`1000`. |
| `message.permissions.kick_protection` | number | Required | Protection against being kicked from the server. Whole number `0`–`1000`. |
| `message.permissions.ban_power` | number | Required | Power to ban a client from the server. Whole number `0`–`1000`. |
| `message.permissions.ban_protection` | number | Required | Protection against a server ban. Whole number `0`–`1000`. |
| `message.permissions.group_management_power` | number | Required | Power to manage server groups and memberships. Whole number `0`–`1000`. |
| `message.permissions.group_protection` | number | Required | Protection threshold of the server group. Whole number `0`–`1000`. |
| `message.permissions.talk_power` | number | Required | Power to transmit channel voice. Whole number `0`–`1000`. |
| `message.permissions.channel_message_power` | number | Required | Power to send channel text. Whole number `0`–`1000`. |
| `message.permissions.channel_picture_power` | number | Required | Power to send channel pictures. Whole number `0`–`1000`. |
| `message.permissions.stream_power` | number | Required | Power to stream channel video. Whole number `0`–`1000`. |
| `message.permissions.move_power` | number | Required | Power to move another client between channels. Whole number `0`–`1000`. |
| `message.permissions.move_protection` | number | Required | Protection against being moved. Whole number `0`–`1000`. |
| `message.permissions.moderation_power` | number | Required | Power to moderate another client. Whole number `0`–`1000`. |
| `message.permissions.moderation_protection` | number | Required | Protection against client moderation. Whole number `0`–`1000`. |
| `message.permissions.permission_edit_power` | number | Required | Power to edit permission definitions. Whole number `0`–`1000`. |
| `message.permissions.grants` | object | Required | Editing thresholds keyed by each supported action or power name; values are whole numbers `0`–`1000`. |
| `message.permissions.grants.create_channel` | number | Required | Editing threshold for `create_channel`. |
| `message.permissions.grants.create_temp_channel` | number | Required | Editing threshold for `create_temp_channel`. |
| `message.permissions.grants.kick` | number | Required | Editing threshold for `kick`. |
| `message.permissions.grants.ban` | number | Required | Editing threshold for `ban`. |
| `message.permissions.grants.view_stored_users` | number | Required | Editing threshold for `view_stored_users`. |
| `message.permissions.grants.view_group_permissions` | number | Required | Editing threshold for `view_group_permissions`. |
| `message.permissions.grants.view_group_members` | number | Required | Editing threshold for `view_group_members`. |
| `message.permissions.grants.create_groups` | number | Required | Editing threshold for `create_groups`. |
| `message.permissions.grants.edit_groups` | number | Required | Editing threshold for `edit_groups`. |
| `message.permissions.grants.delete_groups` | number | Required | Editing threshold for `delete_groups`. |
| `message.permissions.grants.assign_groups` | number | Required | Editing threshold for `assign_groups`. |
| `message.permissions.grants.remove_groups` | number | Required | Editing threshold for `remove_groups`. |
| `message.permissions.grants.edit_group_permissions` | number | Required | Editing threshold for `edit_group_permissions`. |
| `message.permissions.grants.upload_icons` | number | Required | Editing threshold for `upload_icons`. |
| `message.permissions.grants.view_client_details` | number | Required | Editing threshold for `view_client_details`. |
| `message.permissions.grants.view_server_logs` | number | Required | Editing threshold for `view_server_logs`. |
| `message.permissions.grants.view_server_info` | number | Required | Editing threshold for `view_server_info`. |
| `message.permissions.grants.send_offline_messages` | number | Required | Editing threshold for `send_offline_messages`. |
| `message.permissions.grants.receive_offline_messages` | number | Required | Editing threshold for `receive_offline_messages`. |
| `message.permissions.grants.message_idle_clients` | number | Required | Editing threshold for `message_idle_clients`. |
| `message.permissions.grants.call_idle_clients` | number | Required | Editing threshold for `call_idle_clients`. |
| `message.permissions.grants.delete_icons` | number | Required | Editing threshold for `delete_icons`. |
| `message.permissions.grants.upload_emojis` | number | Required | Editing threshold for `upload_emojis`. |
| `message.permissions.grants.delete_emojis` | number | Required | Editing threshold for `delete_emojis`. |
| `message.permissions.grants.view_ip_addresses` | number | Required | Editing threshold for `view_ip_addresses`. |
| `message.permissions.grants.view_bans` | number | Required | Editing threshold for `view_bans`. |
| `message.permissions.grants.remove_bans` | number | Required | Editing threshold for `remove_bans`. |
| `message.permissions.grants.move_clients` | number | Required | Editing threshold for `move_clients`. |
| `message.permissions.grants.kick_from_channel` | number | Required | Editing threshold for `kick_from_channel`. |
| `message.permissions.grants.mute_clients` | number | Required | Editing threshold for `mute_clients`. |
| `message.permissions.grants.stop_client_streams` | number | Required | Editing threshold for `stop_client_streams`. |
| `message.permissions.grants.edit_channel_name` | number | Required | Editing threshold for `edit_channel_name`. |
| `message.permissions.grants.edit_channel_description` | number | Required | Editing threshold for `edit_channel_description`. |
| `message.permissions.grants.edit_channel_password` | number | Required | Editing threshold for `edit_channel_password`. |
| `message.permissions.grants.edit_channel_powers` | number | Required | Editing threshold for `edit_channel_powers`. |
| `message.permissions.grants.edit_channel_options` | number | Required | Editing threshold for `edit_channel_options`. |
| `message.permissions.grants.delete_channel` | number | Required | Editing threshold for `delete_channel`. |
| `message.permissions.grants.assign_channel_groups` | number | Required | Editing threshold for `assign_channel_groups`. |
| `message.permissions.grants.send_direct_pictures` | number | Required | Editing threshold for `send_direct_pictures`. |
| `message.permissions.grants.send_direct_files` | number | Required | Editing threshold for `send_direct_files`. |
| `message.permissions.grants.send_channel_pictures` | number | Required | Editing threshold for `send_channel_pictures`. |
| `message.permissions.grants.send_channel_files` | number | Required | Editing threshold for `send_channel_files`. |
| `message.permissions.grants.create_music_bots` | number | Required | Editing threshold for `create_music_bots`. |
| `message.permissions.grants.delete_music_bots` | number | Required | Editing threshold for `delete_music_bots`. |
| `message.permissions.grants.upload_music_tracks` | number | Required | Editing threshold for `upload_music_tracks`. |
| `message.permissions.grants.delete_music_tracks` | number | Required | Editing threshold for `delete_music_tracks`. |
| `message.permissions.grants.control_music_playback` | number | Required | Editing threshold for `control_music_playback`. |
| `message.permissions.grants.use_reserved_slots` | number | Required | Editing threshold for `use_reserved_slots`. |
| `message.permissions.grants.create_channel_groups` | number | Required | Editing threshold for `create_channel_groups`. |
| `message.permissions.grants.edit_channel_groups` | number | Required | Editing threshold for `edit_channel_groups`. |
| `message.permissions.grants.delete_channel_groups` | number | Required | Editing threshold for `delete_channel_groups`. |
| `message.permissions.grants.view_client_connection_time` | number | Required | Editing threshold for `view_client_connection_time`. |
| `message.permissions.grants.bypass_channel_password` | number | Required | Editing threshold for `bypass_channel_password`. |
| `message.permissions.grants.kick_power` | number | Required | Editing threshold for `kick_power`. |
| `message.permissions.grants.kick_protection` | number | Required | Editing threshold for `kick_protection`. |
| `message.permissions.grants.ban_power` | number | Required | Editing threshold for `ban_power`. |
| `message.permissions.grants.ban_protection` | number | Required | Editing threshold for `ban_protection`. |
| `message.permissions.grants.group_management_power` | number | Required | Editing threshold for `group_management_power`. |
| `message.permissions.grants.group_protection` | number | Required | Editing threshold for `group_protection`. |
| `message.permissions.grants.talk_power` | number | Required | Editing threshold for `talk_power`. |
| `message.permissions.grants.channel_message_power` | number | Required | Editing threshold for `channel_message_power`. |
| `message.permissions.grants.channel_picture_power` | number | Required | Editing threshold for `channel_picture_power`. |
| `message.permissions.grants.stream_power` | number | Required | Editing threshold for `stream_power`. |
| `message.permissions.grants.move_power` | number | Required | Editing threshold for `move_power`. |
| `message.permissions.grants.move_protection` | number | Required | Editing threshold for `move_protection`. |
| `message.permissions.grants.moderation_power` | number | Required | Editing threshold for `moderation_power`. |
| `message.permissions.grants.moderation_protection` | number | Required | Editing threshold for `moderation_protection`. |
| `message.permissions.grants.permission_edit_power` | number | Required | Editing threshold for `permission_edit_power`. |
| `message.permissions.flags` | object | Optional | Optional flags keyed by supported action/power names. Only nonzero flag sets are emitted. Unsupported flag names, including `skip` where unavailable, are invalid even when false. |
| `message.permissions.flags.create_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_temp_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_temp_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_temp_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick`. An action needs an explicit non-default decision. |
| `message.permissions.flags.kick.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.ban` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban`. An action needs an explicit non-default decision. |
| `message.permissions.flags.ban.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_stored_users` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_stored_users`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_stored_users.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_group_permissions` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_group_permissions`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_group_permissions.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_group_members` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_group_members`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_group_members.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.assign_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.assign_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.remove_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `remove_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.remove_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_group_permissions` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_group_permissions`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_group_permissions.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.upload_icons` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_icons`. An action needs an explicit non-default decision. |
| `message.permissions.flags.upload_icons.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_client_details` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_client_details`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_client_details.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_server_logs` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_server_logs`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_server_logs.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_server_info` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_server_info`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_server_info.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_offline_messages` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_offline_messages`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_offline_messages.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.receive_offline_messages` | object | Optional; only nonzero flag sets are emitted | Flag object for `receive_offline_messages`. An action needs an explicit non-default decision. |
| `message.permissions.flags.receive_offline_messages.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.message_idle_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `message_idle_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.message_idle_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.call_idle_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `call_idle_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.call_idle_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_icons` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_icons`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_icons.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.upload_emojis` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_emojis`. An action needs an explicit non-default decision. |
| `message.permissions.flags.upload_emojis.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_emojis` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_emojis`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_emojis.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_ip_addresses` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_ip_addresses`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_ip_addresses.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_bans` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_bans`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_bans.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.remove_bans` | object | Optional; only nonzero flag sets are emitted | Flag object for `remove_bans`. An action needs an explicit non-default decision. |
| `message.permissions.flags.remove_bans.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.move_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.move_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick_from_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_from_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.kick_from_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick_from_channel.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.mute_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `mute_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.mute_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.mute_clients.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.stop_client_streams` | object | Optional; only nonzero flag sets are emitted | Flag object for `stop_client_streams`. An action needs an explicit non-default decision. |
| `message.permissions.flags.stop_client_streams.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.stop_client_streams.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_name` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_name`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_name.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_name.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_description` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_description`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_description.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_description.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_password`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_password.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_powers` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_powers`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_powers.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_powers.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.edit_channel_options` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_options`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_options.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_options.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.delete_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_channel.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.assign_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.assign_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.assign_channel_groups.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.send_direct_pictures` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_direct_pictures`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_direct_pictures.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_direct_files` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_direct_files`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_direct_files.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_channel_pictures` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_channel_pictures`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_channel_pictures.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.send_channel_files` | object | Optional; only nonzero flag sets are emitted | Flag object for `send_channel_files`. An action needs an explicit non-default decision. |
| `message.permissions.flags.send_channel_files.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_music_bots` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_music_bots`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_music_bots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_music_bots` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_music_bots`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_music_bots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.upload_music_tracks` | object | Optional; only nonzero flag sets are emitted | Flag object for `upload_music_tracks`. An action needs an explicit non-default decision. |
| `message.permissions.flags.upload_music_tracks.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_music_tracks` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_music_tracks`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_music_tracks.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.control_music_playback` | object | Optional; only nonzero flag sets are emitted | Flag object for `control_music_playback`. An action needs an explicit non-default decision. |
| `message.permissions.flags.control_music_playback.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.use_reserved_slots` | object | Optional; only nonzero flag sets are emitted | Flag object for `use_reserved_slots`. An action needs an explicit non-default decision. |
| `message.permissions.flags.use_reserved_slots.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.create_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `create_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.create_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.view_client_connection_time` | object | Optional; only nonzero flag sets are emitted | Flag object for `view_client_connection_time`. An action needs an explicit non-default decision. |
| `message.permissions.flags.view_client_connection_time.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.bypass_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `bypass_channel_password`. An action needs an explicit non-default decision. |
| `message.permissions.flags.bypass_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.bypass_channel_password.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.kick_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_power`. Numeric power flags. |
| `message.permissions.flags.kick_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.kick_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_protection`. Numeric power flags. |
| `message.permissions.flags.kick_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.ban_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban_power`. Numeric power flags. |
| `message.permissions.flags.ban_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.ban_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `ban_protection`. Numeric power flags. |
| `message.permissions.flags.ban_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.group_management_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `group_management_power`. Numeric power flags. |
| `message.permissions.flags.group_management_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.group_management_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.group_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `group_protection`. Numeric power flags. |
| `message.permissions.flags.group_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.group_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.talk_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `talk_power`. Numeric power flags. |
| `message.permissions.flags.talk_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.talk_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.channel_message_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_message_power`. Numeric power flags. |
| `message.permissions.flags.channel_message_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_message_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.channel_picture_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_picture_power`. Numeric power flags. |
| `message.permissions.flags.channel_picture_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_picture_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.stream_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `stream_power`. Numeric power flags. |
| `message.permissions.flags.stream_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.stream_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.move_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_power`. Numeric power flags. |
| `message.permissions.flags.move_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.move_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.move_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `move_protection`. Numeric power flags. |
| `message.permissions.flags.move_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.move_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.moderation_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `moderation_power`. Numeric power flags. |
| `message.permissions.flags.moderation_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.moderation_power.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.moderation_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `moderation_protection`. Numeric power flags. |
| `message.permissions.flags.moderation_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.moderation_protection.skip` | boolean | Optional; absent means false | Whether this server-group value skips the channel-group counterpart. |
| `message.permissions.flags.permission_edit_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `permission_edit_power`. Numeric power flags. |
| `message.permissions.flags.permission_edit_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |

### Server response: `identity_list`

**Body**

```json
{
    "message":
    {
        "type": "identity_list",
        "identities":
        [
            {
                "public_key_hash": "<base64 identity hash>",
                "username": "Alice",
                "group_ids":
                [2],
                "is_online": false
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"identity_list"`. |
| `message.identities` | object[] | Required | Stored identity records; may be empty. |
| `message.identities[].public_key_hash` | string | Required | Stored public-key identity hash. This field is named `public_key_hash`, not `identity_hash`. |
| `message.identities[].username` | string | Required | Client display name. |
| `message.identities[].group_ids` | number[] | Required | Server-group IDs; `[]` means no listed memberships. |
| `message.identities[].is_online` | boolean | Required | Whether this identity has an online client. |

## Channel moderation bodies

| Direction | Type |
| --- | --- |
| Client request | [`channel_access_request`](#client-request-channel_access_request) |
| Client request | [`moderate_client`](#client-request-moderate_client) |
| Client request | [`edit_channel_group`](#client-request-edit_channel_group) |
| Client request | [`manage_bans`](#client-request-manage_bans) |
| Server response | [`moderation_result`](#server-response-moderation_result) |
| Server response | [`channel_access`](#server-response-channel_access) |
| Server response | [`client_context_access`](#server-response-client_context_access) |
| Server response | [`channel_group_changed`](#server-response-channel_group_changed) |
| Server response | [`ban_list`](#server-response-ban_list) |

### Client request: `channel_access_request`

**Body**

```json
{
    "message":
    {
        "type": "channel_access_request",
        "channel_id": 3,
        "client_id": 7,
        "identity": "<RSA public key>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_access_request"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.client_id` | number | Optional; paired with identity for target context | Numeric client slot ID. |
| `message.identity` | string | Optional; paired with client_id for target context | Current client RSA public key, not the identity hash. |

**Channel-only body**

```json
{
    "message":
    {
        "type": "channel_access_request",
        "channel_id": 3
    }
}
```

### Client request: `moderate_client`

The action selects the body variant. Additional fields are listed separately for each action.

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"moderate_client"`. |
| `message.action` | string | Required | Exactly `"move"`, `"kick_from_channel"`, `"mute"`, `"stop_stream"` or `"channel_group"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.identity` | string | Required | Current client RSA public key, not the identity hash. |
| `message.destination_channel_id` | number | Required only for move | Destination channel ID. |
| `message.duration_seconds` | number | Required only for mute | Mute duration as a whole number `0`–`86400`; `0` removes the mute. |
| `message.role` | number | Required only for channel_group | Requested channel-group ID: `0` ordinary member, `1` Channel Admin, `2`–`127` custom. |

**move**

```json
{
    "message":
    {
        "type": "moderate_client",
        "action": "move",
        "client_id": 7,
        "channel_id": 3,
        "identity": "<RSA public key>",
        "destination_channel_id": 4
    }
}
```

**kick_from_channel**

```json
{
    "message":
    {
        "type": "moderate_client",
        "action": "kick_from_channel",
        "client_id": 7,
        "channel_id": 3,
        "identity": "<RSA public key>"
    }
}
```

**mute**

```json
{
    "message":
    {
        "type": "moderate_client",
        "action": "mute",
        "client_id": 7,
        "channel_id": 3,
        "identity": "<RSA public key>",
        "duration_seconds": 60
    }
}
```

**stop_stream**

```json
{
    "message":
    {
        "type": "moderate_client",
        "action": "stop_stream",
        "client_id": 7,
        "channel_id": 3,
        "identity": "<RSA public key>"
    }
}
```

**channel_group**

```json
{
    "message":
    {
        "type": "moderate_client",
        "action": "channel_group",
        "client_id": 7,
        "channel_id": 3,
        "identity": "<RSA public key>",
        "role": 2
    }
}
```

**Remove mute**

```json
{
    "message":
    {
        "type": "moderate_client",
        "action": "mute",
        "client_id": 7,
        "channel_id": 3,
        "identity": "<RSA public key>",
        "duration_seconds": 0
    }
}
```

### Client request: `edit_channel_group`

Each action is a stored string decision; numeric powers and `grants` are whole numbers from 0 through 1000. Input is a complete replacement definition: omitted actions, powers and grant thresholds take their defaults. Unknown names are invalid. The full list of supported members is expanded below.

The nested `permissions` object accepts exactly the [11 channel action permissions](permission-reference.md#complete-channel-action-list) and [10 channel power/protection fields](permission-reference.md#channel-power-fields), plus their `grants` and `flags` objects. Channel flags support `negate` only; `skip` is not accepted.

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"edit_channel_group"`. |
| `message.action` | string | Required | Exactly `"create"`, `"update"` or `"delete"`. |
| `message.group_id` | number | Required for update/delete | Channel-group ID. `0` cannot be edited; `1` cannot be deleted. |
| `message.name` | string | Required for create/update | Nonempty channel-group display name, fewer than 32 bytes in UTF-8. |
| `message.icon_id` | number | Required for create/update | `-2` built-in icon, `-1` no icon, or an existing server icon ID. |
| `message.permissions` | object | Required for create/update | Replacement channel-group permission definition. |
| `message.permissions.kick_from_channel` | string | Optional; defaults to deny | Stored permission for `kick_from_channel`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.mute_clients` | string | Optional; defaults to deny | Stored permission for `mute_clients`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.stop_client_streams` | string | Optional; defaults to deny | Stored permission for `stop_client_streams`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.edit_channel_description` | string | Optional; defaults to deny | Stored permission for `edit_channel_description`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.edit_channel_name` | string | Optional; defaults to deny | Stored permission for `edit_channel_name`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.edit_channel_password` | string | Optional; defaults to deny | Stored permission for `edit_channel_password`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.edit_channel_powers` | string | Optional; defaults to deny | Stored permission for `edit_channel_powers`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.edit_channel_options` | string | Optional; defaults to deny | Stored permission for `edit_channel_options`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.delete_channel` | string | Optional; defaults to deny | Stored permission for `delete_channel`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.assign_channel_groups` | string | Optional; defaults to deny | Stored permission for `assign_channel_groups`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.bypass_channel_password` | string | Optional; defaults to deny | Stored permission for `bypass_channel_password`. Input accepts `"grant"`, `"deny"` or `"default"`; `"default"` and omission become `"deny"`. |
| `message.permissions.channel_group_assignment_power` | number | Optional; defaults to 0 | Power to assign a channel group. Whole number `0`–`1000`. |
| `message.permissions.channel_group_protection` | number | Optional; defaults to 0 | Protection threshold of the channel group. Whole number `0`–`1000`. |
| `message.permissions.talk_power` | number | Optional; defaults to 0 | Power to transmit channel voice. Whole number `0`–`1000`. |
| `message.permissions.channel_message_power` | number | Optional; defaults to 0 | Power to send channel text. Whole number `0`–`1000`. |
| `message.permissions.channel_picture_power` | number | Optional; defaults to 0 | Power to send channel pictures. Whole number `0`–`1000`. |
| `message.permissions.stream_power` | number | Optional; defaults to 0 | Power to stream channel video. Whole number `0`–`1000`. |
| `message.permissions.channel_kick_power` | number | Optional; defaults to 0 | Power to kick a client from a channel. Whole number `0`–`1000`. |
| `message.permissions.channel_kick_protection` | number | Optional; defaults to 0 | Protection against a channel kick. Whole number `0`–`1000`. |
| `message.permissions.channel_moderation_power` | number | Optional; defaults to 0 | Power to mute a client or stop a stream in a channel. Whole number `0`–`1000`. |
| `message.permissions.channel_moderation_protection` | number | Optional; defaults to 0 | Protection against channel moderation. Whole number `0`–`1000`. |
| `message.permissions.grants` | object | Optional; omitted thresholds default to 1000 | Editing thresholds keyed by each supported action or power name; values are whole numbers `0`–`1000`. |
| `message.permissions.grants.kick_from_channel` | number | Optional; defaults to 1000 | Editing threshold for `kick_from_channel`. |
| `message.permissions.grants.mute_clients` | number | Optional; defaults to 1000 | Editing threshold for `mute_clients`. |
| `message.permissions.grants.stop_client_streams` | number | Optional; defaults to 1000 | Editing threshold for `stop_client_streams`. |
| `message.permissions.grants.edit_channel_description` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_description`. |
| `message.permissions.grants.edit_channel_name` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_name`. |
| `message.permissions.grants.edit_channel_password` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_password`. |
| `message.permissions.grants.edit_channel_powers` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_powers`. |
| `message.permissions.grants.edit_channel_options` | number | Optional; defaults to 1000 | Editing threshold for `edit_channel_options`. |
| `message.permissions.grants.delete_channel` | number | Optional; defaults to 1000 | Editing threshold for `delete_channel`. |
| `message.permissions.grants.assign_channel_groups` | number | Optional; defaults to 1000 | Editing threshold for `assign_channel_groups`. |
| `message.permissions.grants.bypass_channel_password` | number | Optional; defaults to 1000 | Editing threshold for `bypass_channel_password`. |
| `message.permissions.grants.channel_group_assignment_power` | number | Optional; defaults to 1000 | Editing threshold for `channel_group_assignment_power`. |
| `message.permissions.grants.channel_group_protection` | number | Optional; defaults to 1000 | Editing threshold for `channel_group_protection`. |
| `message.permissions.grants.talk_power` | number | Optional; defaults to 1000 | Editing threshold for `talk_power`. |
| `message.permissions.grants.channel_message_power` | number | Optional; defaults to 1000 | Editing threshold for `channel_message_power`. |
| `message.permissions.grants.channel_picture_power` | number | Optional; defaults to 1000 | Editing threshold for `channel_picture_power`. |
| `message.permissions.grants.stream_power` | number | Optional; defaults to 1000 | Editing threshold for `stream_power`. |
| `message.permissions.grants.channel_kick_power` | number | Optional; defaults to 1000 | Editing threshold for `channel_kick_power`. |
| `message.permissions.grants.channel_kick_protection` | number | Optional; defaults to 1000 | Editing threshold for `channel_kick_protection`. |
| `message.permissions.grants.channel_moderation_power` | number | Optional; defaults to 1000 | Editing threshold for `channel_moderation_power`. |
| `message.permissions.grants.channel_moderation_protection` | number | Optional; defaults to 1000 | Editing threshold for `channel_moderation_protection`. |
| `message.permissions.flags` | object | Optional | Optional flags keyed by supported action/power names. Only nonzero flag sets are emitted. Unsupported flag names, including `skip` where unavailable, are invalid even when false. |
| `message.permissions.flags.kick_from_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `kick_from_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.kick_from_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.mute_clients` | object | Optional; only nonzero flag sets are emitted | Flag object for `mute_clients`. An action needs an explicit non-default decision. |
| `message.permissions.flags.mute_clients.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.stop_client_streams` | object | Optional; only nonzero flag sets are emitted | Flag object for `stop_client_streams`. An action needs an explicit non-default decision. |
| `message.permissions.flags.stop_client_streams.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_description` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_description`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_description.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_name` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_name`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_name.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_password`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_powers` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_powers`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_powers.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.edit_channel_options` | object | Optional; only nonzero flag sets are emitted | Flag object for `edit_channel_options`. An action needs an explicit non-default decision. |
| `message.permissions.flags.edit_channel_options.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.delete_channel` | object | Optional; only nonzero flag sets are emitted | Flag object for `delete_channel`. An action needs an explicit non-default decision. |
| `message.permissions.flags.delete_channel.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.assign_channel_groups` | object | Optional; only nonzero flag sets are emitted | Flag object for `assign_channel_groups`. An action needs an explicit non-default decision. |
| `message.permissions.flags.assign_channel_groups.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.bypass_channel_password` | object | Optional; only nonzero flag sets are emitted | Flag object for `bypass_channel_password`. An action needs an explicit non-default decision. |
| `message.permissions.flags.bypass_channel_password.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_group_assignment_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_group_assignment_power`. Numeric power flags. |
| `message.permissions.flags.channel_group_assignment_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_group_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_group_protection`. Numeric power flags. |
| `message.permissions.flags.channel_group_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.talk_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `talk_power`. Numeric power flags. |
| `message.permissions.flags.talk_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_message_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_message_power`. Numeric power flags. |
| `message.permissions.flags.channel_message_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_picture_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_picture_power`. Numeric power flags. |
| `message.permissions.flags.channel_picture_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.stream_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `stream_power`. Numeric power flags. |
| `message.permissions.flags.stream_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_kick_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_kick_power`. Numeric power flags. |
| `message.permissions.flags.channel_kick_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_kick_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_kick_protection`. Numeric power flags. |
| `message.permissions.flags.channel_kick_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_moderation_power` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_moderation_power`. Numeric power flags. |
| `message.permissions.flags.channel_moderation_power.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |
| `message.permissions.flags.channel_moderation_protection` | object | Optional; only nonzero flag sets are emitted | Flag object for `channel_moderation_protection`. Numeric power flags. |
| `message.permissions.flags.channel_moderation_protection.negate` | boolean | Optional; absent means false | Whether this stored action or power has the negate flag. |

**Create**

```json
{
    "message":
    {
        "type": "edit_channel_group",
        "action": "create",
        "name": "Speakers",
        "icon_id": -1,
        "permissions":
        {
            "kick_from_channel": "deny",
            "mute_clients": "deny",
            "stop_client_streams": "deny",
            "edit_channel_description": "deny",
            "edit_channel_name": "grant",
            "edit_channel_password": "deny",
            "edit_channel_powers": "deny",
            "edit_channel_options": "deny",
            "delete_channel": "deny",
            "assign_channel_groups": "deny",
            "bypass_channel_password": "deny",
            "channel_group_assignment_power": 0,
            "channel_group_protection": 0,
            "talk_power": 0,
            "channel_message_power": 0,
            "channel_picture_power": 0,
            "stream_power": 0,
            "channel_kick_power": 0,
            "channel_kick_protection": 0,
            "channel_moderation_power": 0,
            "channel_moderation_protection": 0,
            "grants":
            {
                "kick_from_channel": 1000,
                "mute_clients": 1000,
                "stop_client_streams": 1000,
                "edit_channel_description": 1000,
                "edit_channel_name": 1000,
                "edit_channel_password": 1000,
                "edit_channel_powers": 1000,
                "edit_channel_options": 1000,
                "delete_channel": 1000,
                "assign_channel_groups": 1000,
                "bypass_channel_password": 1000,
                "channel_group_assignment_power": 1000,
                "channel_group_protection": 1000,
                "talk_power": 1000,
                "channel_message_power": 1000,
                "channel_picture_power": 1000,
                "stream_power": 1000,
                "channel_kick_power": 1000,
                "channel_kick_protection": 1000,
                "channel_moderation_power": 1000,
                "channel_moderation_protection": 1000
            },
            "flags":
            {
                "edit_channel_name":
                {
                    "negate": true
                }
            }
        }
    }
}
```

**Update**

```json
{
    "message":
    {
        "type": "edit_channel_group",
        "action": "update",
        "group_id": 2,
        "name": "Speakers",
        "icon_id": -1,
        "permissions":
        {
            "kick_from_channel": "deny",
            "mute_clients": "deny",
            "stop_client_streams": "deny",
            "edit_channel_description": "deny",
            "edit_channel_name": "grant",
            "edit_channel_password": "deny",
            "edit_channel_powers": "deny",
            "edit_channel_options": "deny",
            "delete_channel": "deny",
            "assign_channel_groups": "deny",
            "bypass_channel_password": "deny",
            "channel_group_assignment_power": 0,
            "channel_group_protection": 0,
            "talk_power": 0,
            "channel_message_power": 0,
            "channel_picture_power": 0,
            "stream_power": 0,
            "channel_kick_power": 0,
            "channel_kick_protection": 0,
            "channel_moderation_power": 0,
            "channel_moderation_protection": 0,
            "grants":
            {
                "kick_from_channel": 1000,
                "mute_clients": 1000,
                "stop_client_streams": 1000,
                "edit_channel_description": 1000,
                "edit_channel_name": 1000,
                "edit_channel_password": 1000,
                "edit_channel_powers": 1000,
                "edit_channel_options": 1000,
                "delete_channel": 1000,
                "assign_channel_groups": 1000,
                "bypass_channel_password": 1000,
                "channel_group_assignment_power": 1000,
                "channel_group_protection": 1000,
                "talk_power": 1000,
                "channel_message_power": 1000,
                "channel_picture_power": 1000,
                "stream_power": 1000,
                "channel_kick_power": 1000,
                "channel_kick_protection": 1000,
                "channel_moderation_power": 1000,
                "channel_moderation_protection": 1000
            },
            "flags":
            {
                "edit_channel_name":
                {
                    "negate": true
                }
            }
        }
    }
}
```

**Delete**

```json
{
    "message":
    {
        "type": "edit_channel_group",
        "action": "delete",
        "group_id": 2
    }
}
```

### Client request: `manage_bans`

**Body**

```json
{
    "message":
    {
        "type": "manage_bans",
        "remove_token": "0123456789abcdef0123456789abcdef"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"manage_bans"`. |
| `message.remove_token` | string | Optional | 32-character removal token from a ban entry. Omission is a list-only body. |

**List-only body**

```json
{
    "message":
    {
        "type": "manage_bans"
    }
}
```

### Server response: `moderation_result`

**Body**

```json
{
    "message":
    {
        "type": "moderation_result",
        "succeeded": true,
        "text": "Client moved."
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"moderation_result"`. |
| `message.succeeded` | boolean | Required | True for success; false for failure. |
| `message.text` | string | Required | Readable result or notice text, not a fixed code enum. |

**Failure body**

```json
{
    "message":
    {
        "type": "moderation_result",
        "succeeded": false,
        "text": "Insufficient permissions."
    }
}
```

**Administrator password and ban result bodies**

The same response body acknowledges `change_admin_password`, `ban` and the address-based `remove_ban`. The `succeeded` value is `true` only after the corresponding change has been saved. These responses do not contain passwords, hashes or banned identities.

```json
{
    "message":
    {
        "type": "moderation_result",
        "succeeded": false,
        "text": "The admin password was not changed because the settings could not be saved."
    }
}
```

| Request/result | `message.succeeded` | `message.text` |
| --- | --- | --- |
| Password saved | `true` | `Admin password changed.` |
| Password save failed | `false` | `The admin password was not changed because the settings could not be saved.` |
| Ban saved | `true` | `Client banned.` |
| Ban list full | `false` | `The client was not banned because the ban list is full.` |
| Ban save failed | `false` | `The client was not banned because the settings could not be saved.` |
| All bans for the IP removed | `true` | `Bans for this IP address removed.` |
| IP unban save failed | `false` | `The bans were not removed because the settings could not be saved.` |
| No matching IP ban | `false` | `No bans were found for this IP address.` |

A rejected channel-array reduction also sends this body with `succeeded: false` and `text: "Delete channels whose IDs are outside the new channel count before lowering it."`, followed by `server_settings_saved` with `success: false`.

### Server response: `channel_access`

**Body**

```json
{
    "message":
    {
        "type": "channel_access",
        "channel_id": 3,
        "is_temp_channel": false,
        "can_make_temporary": true,
        "temporary_type_error": "",
        "can_make_permanent": true,
        "permanent_type_error": "",
        "can_bypass_password": false,
        "role": 0,
        "rights":
        {
            "edit_channel_name": false,
            "edit_channel_description": false,
            "edit_channel_password": false,
            "edit_channel_powers": false,
            "edit_channel_options": false,
            "delete_channel": false,
            "assign_channel_groups": false,
            "move_clients": false,
            "kick_from_channel": false,
            "mute_clients": false,
            "stop_client_streams": false
        },
        "powers":
        {
            "talk_power": 25,
            "channel_message_power": 25,
            "channel_picture_power": 25,
            "stream_power": 25
        }
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_access"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.is_temp_channel` | boolean | Required | Whether the channel is temporary. |
| `message.can_make_temporary` | boolean | Required | Whether temporary conversion is available. |
| `message.temporary_type_error` | string | Required | Conversion restriction text; `""` means no temporary-conversion error. |
| `message.can_make_permanent` | boolean | Required | Whether permanent conversion is available. |
| `message.permanent_type_error` | string | Required | Conversion restriction text; `""` means no permanent-conversion error. |
| `message.can_bypass_password` | boolean | Required | Whether the receiver may bypass this channel's password. |
| `message.role` | number | Required | Receiver's role in this channel, `0`–`127`. |
| `message.rights` | object | Required | Effective action rights for this channel. |
| `message.rights.edit_channel_name` | boolean | Required | Whether `edit_channel_name` is allowed for the receiver in this channel. |
| `message.rights.edit_channel_description` | boolean | Required | Whether `edit_channel_description` is allowed for the receiver in this channel. |
| `message.rights.edit_channel_password` | boolean | Required | Whether `edit_channel_password` is allowed for the receiver in this channel. |
| `message.rights.edit_channel_powers` | boolean | Required | Whether `edit_channel_powers` is allowed for the receiver in this channel. |
| `message.rights.edit_channel_options` | boolean | Required | Whether `edit_channel_options` is allowed for the receiver in this channel. |
| `message.rights.delete_channel` | boolean | Required | Whether `delete_channel` is allowed for the receiver in this channel. |
| `message.rights.assign_channel_groups` | boolean | Required | Whether `assign_channel_groups` is allowed for the receiver in this channel. |
| `message.rights.move_clients` | boolean | Required | Whether `move_clients` is allowed for the receiver in this channel. |
| `message.rights.kick_from_channel` | boolean | Required | Whether `kick_from_channel` is allowed for the receiver in this channel. |
| `message.rights.mute_clients` | boolean | Required | Whether `mute_clients` is allowed for the receiver in this channel. |
| `message.rights.stop_client_streams` | boolean | Required | Whether `stop_client_streams` is allowed for the receiver in this channel. |
| `message.powers` | object | Required | Effective content powers for this channel. |
| `message.powers.talk_power` | number | Required | Power to transmit channel voice. Whole number `0`–`1000`. |
| `message.powers.channel_message_power` | number | Required | Power to send channel text. Whole number `0`–`1000`. |
| `message.powers.channel_picture_power` | number | Required | Power to send channel pictures. Whole number `0`–`1000`. |
| `message.powers.stream_power` | number | Required | Power to stream channel video. Whole number `0`–`1000`. |

### Server response: `client_context_access`

**Body**

```json
{
    "message":
    {
        "type": "client_context_access",
        "client_id": 7,
        "channel_id": 3,
        "identity": "<RSA public key>",
        "actions":
        {
            "move": false,
            "kick_from_channel": false,
            "mute": false,
            "stop_stream": false
        },
        "channel_groups":
        [0, 2]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"client_context_access"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.identity` | string | Required | Current client RSA public key, not the identity hash. |
| `message.actions` | object | Required | Receiver's available actions against the identified target. |
| `message.actions.move` | boolean | Required | Whether `move` is allowed against this target. |
| `message.actions.kick_from_channel` | boolean | Required | Whether `kick_from_channel` is allowed against this target. |
| `message.actions.mute` | boolean | Required | Whether `mute` is allowed against this target. |
| `message.actions.stop_stream` | boolean | Required | Whether `stop_stream` is allowed against this target. |
| `message.channel_groups` | number[] | Required | Channel roles the receiver may assign to the target. `[]` means none; `0` means ordinary member. |

### Server response: `channel_group_changed`

**Body**

```json
{
    "message":
    {
        "type": "channel_group_changed",
        "client_id": 7,
        "channel_id": 3,
        "channel_group_id": 0
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"channel_group_changed"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.channel_group_id` | number | Required | Channel role ID: `0` ordinary member, `1` Channel Admin, `2`–`127` a custom role. |

### Server response: `ban_list`

**Body**

```json
{
    "message":
    {
        "type": "ban_list",
        "entries":
        [
            {
                "token": "0123456789abcdef0123456789abcdef",
                "timestamp": 1793520000000,
                "identity": "<recorded identity>",
                "ip_address": "192.0.2.8"
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"ban_list"`. |
| `message.entries` | object[] | Required | Ban records; may be empty. |
| `message.entries[].token` | string | Required | 32-character opaque removal token. |
| `message.entries[].timestamp` | number | Required | Ban timestamp in Unix milliseconds. |
| `message.entries[].identity` | string | Required | Identity string recorded with the ban. |
| `message.entries[].ip_address` | string | Conditional: view_ip_addresses | Banned IP address; omitted without the IP-view permission. |

## Server settings and public information

| Direction | Type |
| --- | --- |
| Client request | [`load_server_settings`](#client-request-load_server_settings) |
| Client request | [`save_server_settings`](#client-request-save_server_settings) |
| Client request | [`load_server_info`](#client-request-load_server_info) |
| Client request | [`set_server_banner`](#client-request-set_server_banner) |
| Server response | [`server_settings_values`](#server-response-server_settings_values) |
| Server response | [`server_settings_saved`](#server-response-server_settings_saved) |
| Server response | [`server_policy`](#server-response-server_policy) |
| Server response | [`server_info`](#server-response-server_info) |
| Server response | [`server_banner_result`](#server-response-server_banner_result) |

### Client request: `load_server_settings`

**Body**

```json
{
    "message":
    {
        "type": "load_server_settings",
        "memory_only": false
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"load_server_settings"`. |
| `message.memory_only` | boolean | Optional; defaults to false | True selects the usage-only response shape; false or omission selects the full settings shape. |

**Usage-only request**

```json
{
    "message":
    {
        "type": "load_server_settings",
        "memory_only": true
    }
}
```

### Client request: `save_server_settings`

All listed settings are optional members of `message`. No nested `settings` object is used. `is_permission_system_enabled`, `is_stored_client_list_allowed`, `is_idle_mode_allowed`, `is_avatar_upload_allowed` and `server_banner` are not writable members of this request.

**Body**

```json
{
    "message":
    {
        "type": "save_server_settings",
        "is_server_uptime_published": true,
        "is_server_slots_published": true,
        "is_server_banner_published": true,
        "is_server_features_published": true,
        "is_display_country_flags_active": true,
        "is_admin_country_flag_hidden": false,
        "is_voice_chat_active": true,
        "is_music_bot_audio_active": true,
        "is_video_streaming_active": true,
        "is_hide_clients_in_password_protected_channels_active": false,
        "is_temp_channel_creation_allowed": true,
        "is_typing_indicator_allowed": true,
        "is_client_rename_allowed": true,
        "is_link_preview_allowed": true,
        "is_sending_text_to_idle_clients_allowed": true,
        "is_private_messaging_allowed": true,
        "is_same_ip_address_allowed": true,
        "is_fast_reconnect_allowed": true,
        "is_identity_takeover_allowed": true,
        "is_websocket_ping_active": true,
        "is_connection_check_enforced": true,
        "is_channel_key_reuse_enabled": true,
        "is_channel_key_reuse_on_join_allowed": true,
        "is_channel_key_reuse_on_leave_allowed": true,
        "is_music_bot_marquee_visible_to_everyone": true,
        "is_minimum_rsa_key_bits_announced": true,
        "is_file_upload_allowed": true,
        "is_chat_picture_allowed": true,
        "is_blocking_direct_chat_pictures_active": false,
        "is_blocking_direct_chat_files_active": false,
        "is_blocking_channel_chat_pictures_active": false,
        "is_blocking_channel_chat_files_active": false,
        "is_country_blocking_active": false,
        "is_logging_client_joins_enabled": true,
        "is_logging_username_changes_enabled": true,
        "is_logging_group_changes_enabled": true,
        "is_logging_server_settings_updates_enabled": true,
        "is_logging_kicks_and_bans_enabled": true,
        "is_logging_client_disconnects_enabled": true,
        "is_logging_socket_opens_and_closes_enabled": true,
        "is_logging_failed_attempts_enabled": true,
        "reserved_slots": 2,
        "audio_connections": "both",
        "channel_key_reuse_join_min_clients": 30,
        "channel_key_reuse_leave_min_clients": 30,
        "max_concurrent_logins": 8,
        "webrtc_datachannel_cooldown_seconds": 10,
        "icon_max_size_bytes": 30000,
        "avatar_max_size_kb": 90,
        "minimum_rsa_key_bits": 2048,
        "file_upload_max_size_mb": 10,
        "chat_picture_max_size_mb": 5,
        "blocked_countries":
        ["US"],
        "admin_log_max_size_mb": 10,
        "admin_log_retention_days": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"save_server_settings"`. |
| `message.is_server_uptime_published` | boolean | Optional; absent retains current value | Whether public info includes uptime. |
| `message.is_server_slots_published` | boolean | Optional; absent retains current value | Whether public info includes occupied and maximum slots. |
| `message.is_server_banner_published` | boolean | Optional; absent retains current value | Whether the server banner is public. |
| `message.is_server_features_published` | boolean | Optional; absent retains current value | Whether public info includes feature booleans. |
| `message.is_display_country_flags_active` | boolean | Optional; absent retains current value | Whether client country flags are displayed. |
| `message.is_admin_country_flag_hidden` | boolean | Optional; absent retains current value | Whether administrator country flags are hidden. |
| `message.is_voice_chat_active` | boolean | Optional; absent retains current value | Whether voice chat is enabled server-wide. |
| `message.is_music_bot_audio_active` | boolean | Optional; absent retains current value | Whether music-bot audio is enabled server-wide. |
| `message.is_video_streaming_active` | boolean | Optional; absent retains current value | Whether video streaming is enabled; this settings field uses active, while login/policy use allowed. |
| `message.is_hide_clients_in_password_protected_channels_active` | boolean | Optional; absent retains current value | Whether membership of password-protected channels is concealed from outsiders. |
| `message.is_temp_channel_creation_allowed` | boolean | Optional; absent retains current value | Whether temporary channel creation is enabled by the general setting. |
| `message.max_channel_count` | number | Optional; absent retains current value | Allocated channel slots, a whole number from 1 to 2147483647, subject to allocation limits. Includes root, permanent and temporary channels; valid IDs are 0 through count minus 1. A reduction is rejected if any existing channel ID is at least the new count. Allocation or save failure preserves the current array and count. |
| `message.is_typing_indicator_allowed` | boolean | Optional; absent retains current value | Whether typing indicators are enabled. |
| `message.is_client_rename_allowed` | boolean | Optional; absent retains current value | Whether client renaming is enabled. |
| `message.is_link_preview_allowed` | boolean | Optional; absent retains current value | Whether URL previews are enabled. |
| `message.is_sending_text_to_idle_clients_allowed` | boolean | Optional; absent retains current value | Whether idle clients may receive text. |
| `message.is_private_messaging_allowed` | boolean | Optional; absent retains current value | Whether private conversations are enabled. |
| `message.is_same_ip_address_allowed` | boolean | Optional; absent retains current value | Whether multiple connections from one IP address are allowed. |
| `message.is_fast_reconnect_allowed` | boolean | Optional; absent retains current value | Whether fast reconnection is enabled. |
| `message.is_identity_takeover_allowed` | boolean | Optional; absent retains current value | Whether a new connection may take over the same identity. |
| `message.is_websocket_ping_active` | boolean | Optional; absent retains current value | Whether WebSocket ping is active. |
| `message.is_connection_check_enforced` | boolean | Optional; absent retains current value | Whether application connection checks are enforced. |
| `message.is_channel_key_reuse_enabled` | boolean | Optional; absent retains current value | Whether channel-key reuse is enabled. |
| `message.is_channel_key_reuse_on_join_allowed` | boolean | Optional; absent retains current value | Whether channel keys may be reused when a client joins. |
| `message.is_channel_key_reuse_on_leave_allowed` | boolean | Optional; absent retains current value | Whether channel keys may be reused when a client leaves. |
| `message.is_music_bot_marquee_visible_to_everyone` | boolean | Optional; absent retains current value | Whether a music bot's song label is visible outside its channel. |
| `message.is_minimum_rsa_key_bits_announced` | boolean | Optional; absent retains current value | Whether the minimum RSA key-size requirement may be disclosed. |
| `message.is_file_upload_allowed` | boolean | Optional; absent retains current value | Whether chat file uploads are enabled. |
| `message.is_chat_picture_allowed` | boolean | Optional; absent retains current value | Whether chat picture uploads are enabled. |
| `message.is_blocking_direct_chat_pictures_active` | boolean | Optional; absent retains current value | Whether recipient blocking preferences are active for direct-chat pictures. |
| `message.is_blocking_direct_chat_files_active` | boolean | Optional; absent retains current value | Whether recipient blocking preferences are active for direct-chat files. |
| `message.is_blocking_channel_chat_pictures_active` | boolean | Optional; absent retains current value | Whether recipient blocking preferences are active for channel-chat pictures. |
| `message.is_blocking_channel_chat_files_active` | boolean | Optional; absent retains current value | Whether recipient blocking preferences are active for channel-chat files. |
| `message.is_country_blocking_active` | boolean | Optional; absent retains current value | Whether the blocked-country list is active. |
| `message.is_logging_client_joins_enabled` | boolean | Optional; absent retains current value | Whether client joins are logged. |
| `message.is_logging_username_changes_enabled` | boolean | Optional; absent retains current value | Whether username changes are logged. |
| `message.is_logging_group_changes_enabled` | boolean | Optional; absent retains current value | Whether server-group changes are logged. |
| `message.is_logging_server_settings_updates_enabled` | boolean | Optional; absent retains current value | Whether server-settings updates are logged. |
| `message.is_logging_kicks_and_bans_enabled` | boolean | Optional; absent retains current value | Whether kicks and bans are logged. |
| `message.is_logging_client_disconnects_enabled` | boolean | Optional; absent retains current value | Whether client disconnections are logged. |
| `message.is_logging_socket_opens_and_closes_enabled` | boolean | Optional; absent retains current value | Whether socket opens and closes are logged. |
| `message.is_logging_failed_attempts_enabled` | boolean | Optional; absent retains current value | Whether failed attempts are logged. |
| `message.reserved_slots` | number | Optional; absent retains current value | Reserved connection slots; whole count from `0` to one less than the maximum server capacity. |
| `message.audio_connections` | string | Optional; absent retains current value | Exactly `"both"`, `"websocket"` or `"webrtc"`. |
| `message.channel_key_reuse_join_min_clients` | number | Optional; absent retains current value | Client-count threshold; accepted range `30`–`1000000`, stored as a whole number. |
| `message.channel_key_reuse_leave_min_clients` | number | Optional; absent retains current value | Client-count threshold; accepted range `30`–`1000000`, stored as a whole number. |
| `message.max_concurrent_logins` | number | Optional; absent retains current value | Concurrent login limit; clamped to `1`–`64`. |
| `message.webrtc_datachannel_cooldown_seconds` | number | Optional; absent retains current value | Cooldown in seconds; clamped to `0`–`86400`. |
| `message.icon_max_size_bytes` | number | Optional; absent retains current value | Icon size limit in bytes; clamped to `1000`–`30000`. |
| `message.avatar_max_size_kb` | number | Optional; absent retains current value | Avatar size limit in units of 1024 bytes; accepted whole-value range `1`–`90`. |
| `message.minimum_rsa_key_bits` | number | Optional; absent retains current value | Minimum RSA key size in bits; clamped to `2048`–`8192`. |
| `message.file_upload_max_size_mb` | number | Optional; absent retains current value | File limit in MiB (1024 × 1024 bytes); positive input clamped to `1`–`100`. |
| `message.chat_picture_max_size_mb` | number | Optional; absent retains current value | Picture limit in MiB; positive input clamped to `1`–`15`. |
| `message.blocked_countries` | string[] | Optional; absent retains current value | Complete replacement list of two-letter country codes. Valid codes are normalized; `[]` clears the list. |
| `message.admin_log_max_size_mb` | number | Optional; absent retains current value | Log size limit in MiB; positive input clamped to `1`–`100`. |
| `message.admin_log_retention_days` | number | Optional; absent retains current value | Log retention in days; positive input capped at `30`. |

**Single-field body**

```json
{
    "message":
    {
        "type": "save_server_settings",
        "is_typing_indicator_allowed": false
    }
}
```

**No setting members**

```json
{
    "message":
    {
        "type": "save_server_settings"
    }
}
```

### Client request: `load_server_info`

**Body**

```json
{
    "message":
    {
        "type": "load_server_info",
        "include_banner": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"load_server_info"`. |
| `message.include_banner` | boolean | Optional; defaults to true | Whether the body asks for published banner content. Explicit false omits image content; omission keeps the default true. |

### Client request: `set_server_banner`

**Body**

```json
{
    "message":
    {
        "type": "set_server_banner",
        "banner": "data:image/png;base64,<banner bytes>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"set_server_banner"`. |
| `message.banner` | string | Required | PNG, JPEG or WebP data URL; decoded image at most 32768 bytes, width 1–2048 pixels and height 1–512 pixels. `""` removes the banner. |

**Remove banner**

```json
{
    "message":
    {
        "type": "set_server_banner",
        "banner": ""
    }
}
```

### Server response: `server_settings_values`

The full example and usage-only example are separate body shapes. `ram_bytes` is explicitly nullable in either shape.

**Body**

```json
{
    "message":
    {
        "type": "server_settings_values",
        "websocket_received_bytes": 123456,
        "websocket_sent_bytes": 234567,
        "webrtc_received_bytes": 345678,
        "webrtc_sent_bytes": 456789,
        "ram_bytes": 67108864,
        "uptime_seconds": 3600,
        "used_slots": 8,
        "max_slots": 100,
        "reserved_slots": 2,
        "is_server_uptime_published": true,
        "is_server_slots_published": true,
        "is_server_banner_published": true,
        "is_server_features_published": true,
        "is_display_country_flags_active": true,
        "is_admin_country_flag_hidden": false,
        "is_voice_chat_active": true,
        "is_music_bot_audio_active": true,
        "is_video_streaming_active": true,
        "is_hide_clients_in_password_protected_channels_active": false,
        "is_temp_channel_creation_allowed": true,
        "is_typing_indicator_allowed": true,
        "is_client_rename_allowed": true,
        "is_link_preview_allowed": true,
        "is_sending_text_to_idle_clients_allowed": true,
        "is_private_messaging_allowed": true,
        "is_same_ip_address_allowed": true,
        "is_fast_reconnect_allowed": true,
        "is_identity_takeover_allowed": true,
        "is_websocket_ping_active": true,
        "is_connection_check_enforced": true,
        "is_channel_key_reuse_enabled": true,
        "is_channel_key_reuse_on_join_allowed": true,
        "is_channel_key_reuse_on_leave_allowed": true,
        "is_music_bot_marquee_visible_to_everyone": true,
        "is_minimum_rsa_key_bits_announced": true,
        "is_file_upload_allowed": true,
        "is_chat_picture_allowed": true,
        "is_blocking_direct_chat_pictures_active": false,
        "is_blocking_direct_chat_files_active": false,
        "is_blocking_channel_chat_pictures_active": false,
        "is_blocking_channel_chat_files_active": false,
        "is_country_blocking_active": false,
        "is_logging_client_joins_enabled": true,
        "is_logging_username_changes_enabled": true,
        "is_logging_group_changes_enabled": true,
        "is_logging_server_settings_updates_enabled": true,
        "is_logging_kicks_and_bans_enabled": true,
        "is_logging_client_disconnects_enabled": true,
        "is_logging_socket_opens_and_closes_enabled": true,
        "is_logging_failed_attempts_enabled": true,
        "audio_connections": "both",
        "channel_key_reuse_join_min_clients": 30,
        "channel_key_reuse_leave_min_clients": 30,
        "max_concurrent_logins": 8,
        "webrtc_datachannel_cooldown_seconds": 10,
        "icon_max_size_bytes": 30000,
        "avatar_max_size_kb": 90,
        "minimum_rsa_key_bits": 2048,
        "file_upload_max_size_mb": 10,
        "chat_picture_max_size_mb": 5,
        "blocked_countries":
        ["US"],
        "admin_log_max_size_mb": 10,
        "admin_log_retention_days": 7,
        "server_banner": "",
        "is_permission_system_enabled": true,
        "is_stored_client_list_allowed": true,
        "bans":
        [
            {
                "ip_address": "192.0.2.8",
                "country_iso_code": "SK",
                "identity": "<recorded identity>",
                "extra_data": "",
                "timestamp_banned": 1793520000000
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_settings_values"`. |
| `message.websocket_received_bytes` | number | Required | Total WebSocket bytes received since startup. |
| `message.websocket_sent_bytes` | number | Required | Total WebSocket bytes sent since startup. |
| `message.webrtc_received_bytes` | number | Required | Total WebRTC bytes received since startup. |
| `message.webrtc_sent_bytes` | number | Required | Total WebRTC bytes sent since startup. |
| `message.ram_bytes` | number or null | Required | Process RAM usage in bytes; `null` when unavailable. |
| `message.uptime_seconds` | number | Required | Server uptime in seconds. |
| `message.used_slots` | number | Required | Occupied or retained connection slots; not only visible authenticated clients. |
| `message.max_slots` | number | Required | Maximum server connection-slot capacity. |
| `message.reserved_slots` | number | Required | Number of reserved connection slots. |
| `message.memory_only` | boolean | Present only in usage-only response | Literal `true` in the usage-only body. Omitted entirely from the full response. |
| `message.is_server_uptime_published` | boolean | Present in full response | Whether public info includes uptime. |
| `message.is_server_slots_published` | boolean | Present in full response | Whether public info includes occupied and maximum slots. |
| `message.is_server_banner_published` | boolean | Present in full response | Whether the server banner is public. |
| `message.is_server_features_published` | boolean | Present in full response | Whether public info includes feature booleans. |
| `message.is_display_country_flags_active` | boolean | Present in full response | Whether client country flags are displayed. |
| `message.is_admin_country_flag_hidden` | boolean | Present in full response | Whether administrator country flags are hidden. |
| `message.is_voice_chat_active` | boolean | Present in full response | Whether voice chat is enabled server-wide. |
| `message.is_music_bot_audio_active` | boolean | Present in full response | Whether music-bot audio is enabled server-wide. |
| `message.is_video_streaming_active` | boolean | Present in full response | Whether video streaming is enabled; this settings field uses active, while login/policy use allowed. |
| `message.is_hide_clients_in_password_protected_channels_active` | boolean | Present in full response | Whether membership of password-protected channels is concealed from outsiders. |
| `message.is_temp_channel_creation_allowed` | boolean | Present in full response | Whether temporary channel creation is enabled by the general setting. |
| `message.max_channel_count` | number | Present in full response | Configured channel array size, including root and temporary channels; defaults to 100. |
| `message.is_typing_indicator_allowed` | boolean | Present in full response | Whether typing indicators are enabled. |
| `message.is_client_rename_allowed` | boolean | Present in full response | Whether client renaming is enabled. |
| `message.is_link_preview_allowed` | boolean | Present in full response | Whether URL previews are enabled. |
| `message.is_sending_text_to_idle_clients_allowed` | boolean | Present in full response | Whether idle clients may receive text. |
| `message.is_private_messaging_allowed` | boolean | Present in full response | Whether private conversations are enabled. |
| `message.is_same_ip_address_allowed` | boolean | Present in full response | Whether multiple connections from one IP address are allowed. |
| `message.is_fast_reconnect_allowed` | boolean | Present in full response | Whether fast reconnection is enabled. |
| `message.is_identity_takeover_allowed` | boolean | Present in full response | Whether a new connection may take over the same identity. |
| `message.is_websocket_ping_active` | boolean | Present in full response | Whether WebSocket ping is active. |
| `message.is_connection_check_enforced` | boolean | Present in full response | Whether application connection checks are enforced. |
| `message.is_channel_key_reuse_enabled` | boolean | Present in full response | Whether channel-key reuse is enabled. |
| `message.is_channel_key_reuse_on_join_allowed` | boolean | Present in full response | Whether channel keys may be reused when a client joins. |
| `message.is_channel_key_reuse_on_leave_allowed` | boolean | Present in full response | Whether channel keys may be reused when a client leaves. |
| `message.is_music_bot_marquee_visible_to_everyone` | boolean | Present in full response | Whether a music bot's song label is visible outside its channel. |
| `message.is_minimum_rsa_key_bits_announced` | boolean | Present in full response | Whether the minimum RSA key-size requirement may be disclosed. |
| `message.is_file_upload_allowed` | boolean | Present in full response | Whether chat file uploads are enabled. |
| `message.is_chat_picture_allowed` | boolean | Present in full response | Whether chat picture uploads are enabled. |
| `message.is_blocking_direct_chat_pictures_active` | boolean | Present in full response | Whether recipient blocking preferences are active for direct-chat pictures. |
| `message.is_blocking_direct_chat_files_active` | boolean | Present in full response | Whether recipient blocking preferences are active for direct-chat files. |
| `message.is_blocking_channel_chat_pictures_active` | boolean | Present in full response | Whether recipient blocking preferences are active for channel-chat pictures. |
| `message.is_blocking_channel_chat_files_active` | boolean | Present in full response | Whether recipient blocking preferences are active for channel-chat files. |
| `message.is_country_blocking_active` | boolean | Present in full response | Whether the blocked-country list is active. |
| `message.is_logging_client_joins_enabled` | boolean | Present in full response | Whether client joins are logged. |
| `message.is_logging_username_changes_enabled` | boolean | Present in full response | Whether username changes are logged. |
| `message.is_logging_group_changes_enabled` | boolean | Present in full response | Whether server-group changes are logged. |
| `message.is_logging_server_settings_updates_enabled` | boolean | Present in full response | Whether server-settings updates are logged. |
| `message.is_logging_kicks_and_bans_enabled` | boolean | Present in full response | Whether kicks and bans are logged. |
| `message.is_logging_client_disconnects_enabled` | boolean | Present in full response | Whether client disconnections are logged. |
| `message.is_logging_socket_opens_and_closes_enabled` | boolean | Present in full response | Whether socket opens and closes are logged. |
| `message.is_logging_failed_attempts_enabled` | boolean | Present in full response | Whether failed attempts are logged. |
| `message.audio_connections` | string | Present in full response | Exactly `"both"`, `"websocket"` or `"webrtc"`. |
| `message.channel_key_reuse_join_min_clients` | number | Present in full response | Client-count threshold; accepted range `30`–`1000000`, stored as a whole number. |
| `message.channel_key_reuse_leave_min_clients` | number | Present in full response | Client-count threshold; accepted range `30`–`1000000`, stored as a whole number. |
| `message.max_concurrent_logins` | number | Present in full response | Concurrent login limit; clamped to `1`–`64`. |
| `message.webrtc_datachannel_cooldown_seconds` | number | Present in full response | Cooldown in seconds; clamped to `0`–`86400`. |
| `message.icon_max_size_bytes` | number | Present in full response | Icon size limit in bytes; clamped to `1000`–`30000`. |
| `message.avatar_max_size_kb` | number | Present in full response | Avatar size limit in units of 1024 bytes; accepted whole-value range `1`–`90`. |
| `message.minimum_rsa_key_bits` | number | Present in full response | Minimum RSA key size in bits; clamped to `2048`–`8192`. |
| `message.file_upload_max_size_mb` | number | Present in full response | File limit in MiB (1024 × 1024 bytes); positive input clamped to `1`–`100`. |
| `message.chat_picture_max_size_mb` | number | Present in full response | Picture limit in MiB; positive input clamped to `1`–`15`. |
| `message.blocked_countries` | string[] | Present in full response | Complete replacement list of two-letter country codes. Valid codes are normalized; `[]` clears the list. |
| `message.admin_log_max_size_mb` | number | Present in full response | Log size limit in MiB; positive input clamped to `1`–`100`. |
| `message.admin_log_retention_days` | number | Present in full response | Log retention in days; positive input capped at `30`. |
| `message.server_banner` | string | Present in full response | Current stored banner string; `""` if none. |
| `message.is_permission_system_enabled` | boolean | Present in full response | Whether the permission system is enabled. |
| `message.is_stored_client_list_allowed` | boolean | Present in full response | Whether stored-user visibility is enabled together with identity storage. |
| `message.bans` | object[] | Present in full response | Administrator ban records; a different schema from `ban_list.entries`. |
| `message.bans[].ip_address` | string | Required | Banned IP address. |
| `message.bans[].country_iso_code` | string | Required | Two-letter country code; `""` when unknown or hidden. |
| `message.bans[].identity` | string | Required | Identity recorded with the ban. |
| `message.bans[].extra_data` | string | Required | Additional stored ban text; can be empty. |
| `message.bans[].timestamp_banned` | number | Required | Ban timestamp in Unix milliseconds. |

**Usage-only response, RAM unavailable**

```json
{
    "message":
    {
        "type": "server_settings_values",
        "websocket_received_bytes": 123456,
        "websocket_sent_bytes": 234567,
        "webrtc_received_bytes": 345678,
        "webrtc_sent_bytes": 456789,
        "ram_bytes": null,
        "uptime_seconds": 3600,
        "used_slots": 8,
        "max_slots": 100,
        "reserved_slots": 2,
        "memory_only": true
    }
}
```

### Server response: `server_settings_saved`

**Body**

```json
{
    "message":
    {
        "type": "server_settings_saved",
        "success": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_settings_saved"`. |
| `message.success` | boolean | Required | Boolean save result. This same body type also represents the server-emoji save result. |

**Failure**

```json
{
    "message":
    {
        "type": "server_settings_saved",
        "success": false
    }
}
```

### Server response: `server_policy`

This body omits login-only `value`, `stun_port` and `is_idle_mode_allowed`. Size values here are bytes, unlike several full-settings fields.

**Body**

```json
{
    "message":
    {
        "type": "server_policy",
        "is_audio_over_websocket_allowed": true,
        "is_audio_over_webrtc_allowed": true,
        "is_voice_chat_active": true,
        "is_music_bot_audio_active": true,
        "is_video_streaming_allowed": true,
        "is_fast_reconnect_allowed": true,
        "is_permission_system_enabled": true,
        "is_stored_client_list_allowed": true,
        "is_connection_check_enforced": true,
        "is_channel_key_reuse_enabled": true,
        "is_channel_key_reuse_on_join_allowed": true,
        "is_channel_key_reuse_on_leave_allowed": true,
        "is_music_bot_marquee_visible_to_everyone": true,
        "is_avatar_upload_allowed": true,
        "is_typing_indicator_allowed": true,
        "is_client_rename_allowed": true,
        "is_link_preview_allowed": true,
        "is_file_upload_allowed": true,
        "is_chat_picture_allowed": true,
        "is_blocking_direct_chat_pictures_active": true,
        "is_blocking_direct_chat_files_active": true,
        "is_blocking_channel_chat_pictures_active": true,
        "is_blocking_channel_chat_files_active": true,
        "channel_key_reuse_join_min_clients": 30,
        "channel_key_reuse_leave_min_clients": 30,
        "avatar_max_size": 92160,
        "icon_max_size": 30000,
        "file_upload_max_size": 10485760,
        "chat_picture_max_size": 5242880
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_policy"`. |
| `message.is_audio_over_websocket_allowed` | boolean | Required | Whether audio WebSocket connections are enabled. |
| `message.is_audio_over_webrtc_allowed` | boolean | Required | Whether WebRTC audio connections are enabled. |
| `message.is_voice_chat_active` | boolean | Required | Whether voice chat is enabled server-wide. |
| `message.is_music_bot_audio_active` | boolean | Required | Whether music-bot audio is enabled server-wide. |
| `message.is_video_streaming_allowed` | boolean | Required | Whether video streaming is enabled server-wide. |
| `message.is_fast_reconnect_allowed` | boolean | Required | Whether fast reconnection is enabled. |
| `message.is_permission_system_enabled` | boolean | Required | Whether the permission system is enabled. |
| `message.is_stored_client_list_allowed` | boolean | Required | Whether stored-user visibility is enabled together with identity storage. |
| `message.is_connection_check_enforced` | boolean | Required | Whether application connection checks are enforced. |
| `message.is_channel_key_reuse_enabled` | boolean | Required | Whether channel-key reuse is enabled. |
| `message.is_channel_key_reuse_on_join_allowed` | boolean | Required | Whether channel keys may be reused when a client joins. |
| `message.is_channel_key_reuse_on_leave_allowed` | boolean | Required | Whether channel keys may be reused when a client leaves. |
| `message.is_music_bot_marquee_visible_to_everyone` | boolean | Required | Whether a music bot's song label is visible outside its channel. |
| `message.is_avatar_upload_allowed` | boolean | Required | Whether avatar uploads are enabled. |
| `message.is_typing_indicator_allowed` | boolean | Required | Whether typing indicators are enabled. |
| `message.is_client_rename_allowed` | boolean | Required | Whether client renaming is enabled. |
| `message.is_link_preview_allowed` | boolean | Required | Whether URL previews are enabled. |
| `message.is_file_upload_allowed` | boolean | Required | Whether chat file uploads are enabled. |
| `message.is_chat_picture_allowed` | boolean | Required | Whether chat picture uploads are enabled. |
| `message.is_blocking_direct_chat_pictures_active` | boolean | Required | Whether recipient blocking preferences are active for direct-chat pictures. |
| `message.is_blocking_direct_chat_files_active` | boolean | Required | Whether recipient blocking preferences are active for direct-chat files. |
| `message.is_blocking_channel_chat_pictures_active` | boolean | Required | Whether recipient blocking preferences are active for channel-chat pictures. |
| `message.is_blocking_channel_chat_files_active` | boolean | Required | Whether recipient blocking preferences are active for channel-chat files. |
| `message.channel_key_reuse_join_min_clients` | number | Required | Minimum channel client count for key reuse on join. |
| `message.channel_key_reuse_leave_min_clients` | number | Required | Minimum channel client count for key reuse on leave. |
| `message.avatar_max_size` | number | Required | Avatar size limit in bytes. |
| `message.icon_max_size` | number | Required | Icon size limit in bytes. |
| `message.file_upload_max_size` | number | Required | Chat file size limit in bytes. |
| `message.chat_picture_max_size` | number | Required | Chat picture size limit in bytes. |

### Server response: `server_info`

**Body**

```json
{
    "message":
    {
        "type": "server_info",
        "visibility":
        {
            "uptime": true,
            "slots": true,
            "banner": true,
            "features": true
        },
        "uptime_seconds": 3600,
        "used_slots": 8,
        "max_slots": 100,
        "server_banner": "data:image/png;base64,<banner bytes>",
        "features":
        {
            "is_voice_chat_active": true,
            "is_music_bot_audio_active": true,
            "is_video_streaming_allowed": true,
            "is_audio_over_websocket_allowed": true,
            "is_audio_over_webrtc_allowed": true,
            "is_idle_mode_allowed": true,
            "is_fast_reconnect_allowed": true,
            "is_connection_check_enforced": true,
            "is_typing_indicator_allowed": true,
            "is_client_rename_allowed": true,
            "is_link_preview_allowed": true,
            "is_avatar_upload_allowed": true,
            "is_private_messaging_allowed": true,
            "is_file_upload_allowed": true,
            "is_chat_picture_allowed": true,
            "is_temp_channel_creation_allowed": true,
            "is_identity_storage_enabled": true,
            "is_stored_client_list_allowed": true,
            "is_last_seen_enabled": true,
            "is_offline_messaging_allowed": true,
            "is_display_country_flags_active": true,
            "is_hide_clients_in_password_protected_channels_active": true,
            "is_sending_text_to_idle_clients_allowed": true,
            "is_music_bot_marquee_visible_to_everyone": true
        }
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_info"`. |
| `message.visibility` | object | Required | Publication flags. This object may be `{}` for a receiver without view permission. |
| `message.visibility.uptime` | boolean | Present when the receiver may view server info | Whether uptime is published. |
| `message.visibility.slots` | boolean | Present when the receiver may view server info | Whether slots is published. |
| `message.visibility.banner` | boolean | Present when the receiver may view server info | Whether banner is published. |
| `message.visibility.features` | boolean | Present when the receiver may view server info | Whether features is published. |
| `message.uptime_seconds` | number | Conditional: uptime is published | Server uptime in seconds. |
| `message.used_slots` | number | Conditional: slots are published | Occupied or retained server connection slots. |
| `message.max_slots` | number | Conditional: slots are published | Maximum server connection capacity. |
| `message.server_banner` | string | Conditional: banner is published and content was requested | Published banner content; `""` explicitly means no stored banner. Omission does not mean an empty banner. |
| `message.banner_changed` | boolean | Conditional: banner invalidation body | Literal true marking a changed published banner; this member does not contain image bytes. |
| `message.features` | object | Conditional: features are published | Public feature flags, each a boolean. |
| `message.features.is_voice_chat_active` | boolean | Required | Whether voice chat is enabled server-wide. |
| `message.features.is_music_bot_audio_active` | boolean | Required | Whether music-bot audio is enabled server-wide. |
| `message.features.is_video_streaming_allowed` | boolean | Required | Whether video streaming is enabled server-wide. |
| `message.features.is_audio_over_websocket_allowed` | boolean | Required | Whether audio WebSocket connections are enabled. |
| `message.features.is_audio_over_webrtc_allowed` | boolean | Required | Whether WebRTC audio connections are enabled. |
| `message.features.is_idle_mode_allowed` | boolean | Required | Whether idle mode is enabled. |
| `message.features.is_fast_reconnect_allowed` | boolean | Required | Whether fast reconnection is enabled. |
| `message.features.is_connection_check_enforced` | boolean | Required | Whether application connection checks are enforced. |
| `message.features.is_typing_indicator_allowed` | boolean | Required | Whether typing indicators are enabled. |
| `message.features.is_client_rename_allowed` | boolean | Required | Whether client renaming is enabled. |
| `message.features.is_link_preview_allowed` | boolean | Required | Whether URL previews are enabled. |
| `message.features.is_avatar_upload_allowed` | boolean | Required | Whether avatar uploads are enabled. |
| `message.features.is_private_messaging_allowed` | boolean | Required | Whether private conversations are enabled. |
| `message.features.is_file_upload_allowed` | boolean | Required | Whether chat file uploads are enabled. |
| `message.features.is_chat_picture_allowed` | boolean | Required | Whether chat picture uploads are enabled. |
| `message.features.is_temp_channel_creation_allowed` | boolean | Required | Whether temporary creation is enabled through the permission system or the general setting. |
| `message.features.is_identity_storage_enabled` | boolean | Required | Whether identity storage is enabled. |
| `message.features.is_stored_client_list_allowed` | boolean | Required | Whether stored-user visibility is enabled together with identity storage. |
| `message.features.is_last_seen_enabled` | boolean | Required | Whether last-seen visibility is effectively enabled with its identity-storage dependencies. |
| `message.features.is_offline_messaging_allowed` | boolean | Required | Whether offline messaging is effectively enabled with identity storage and stored-user visibility. |
| `message.features.is_display_country_flags_active` | boolean | Required | Whether client country flags are displayed. |
| `message.features.is_hide_clients_in_password_protected_channels_active` | boolean | Required | Whether membership of password-protected channels is concealed from outsiders. |
| `message.features.is_sending_text_to_idle_clients_allowed` | boolean | Required | Whether idle clients may receive text. |
| `message.features.is_music_bot_marquee_visible_to_everyone` | boolean | Required | Whether a music bot's song label is visible outside its channel. |

**No view permission**

```json
{
    "message":
    {
        "type": "server_info",
        "visibility": {}
    }
}
```

**All publication switches off**

```json
{
    "message":
    {
        "type": "server_info",
        "visibility":
        {
            "uptime": false,
            "slots": false,
            "banner": false,
            "features": false
        }
    }
}
```

**Published banner invalidation**

```json
{
    "message":
    {
        "type": "server_info",
        "visibility":
        {
            "uptime": false,
            "slots": false,
            "banner": true,
            "features": false
        },
        "banner_changed": true
    }
}
```

### Server response: `server_banner_result`

**Body**

```json
{
    "message":
    {
        "type": "server_banner_result",
        "success": true,
        "error": ""
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_banner_result"`. |
| `message.success` | boolean | Required | Whether the banner operation succeeded. |
| `message.error` | string | Required | `""` on success. Current failures: `not_admin`, `invalid_banner`, `out_of_memory`, `save_failed`, `server_ram_budget_reached`. |

**Invalid banner**

```json
{
    "message":
    {
        "type": "server_banner_result",
        "success": false,
        "error": "invalid_banner"
    }
}
```

## Server emoji bodies

| Direction | Type |
| --- | --- |
| Client request | [`load_server_emojis`](#client-request-load_server_emojis) |
| Client request | [`load_server_emoji`](#client-request-load_server_emoji) |
| Client request | [`upload_server_emoji`](#client-request-upload_server_emoji) |
| Client request | [`delete_server_emoji`](#client-request-delete_server_emoji) |
| Client request | [`save_server_emojis`](#client-request-save_server_emojis) |
| Server response | [`server_emoji_list`](#server-response-server_emoji_list) |
| Server response | [`server_emoji_image`](#server-response-server_emoji_image) |
| Server response | [`server_emoji_added`](#server-response-server_emoji_added) |
| Server response | [`server_emoji_deleted`](#server-response-server_emoji_deleted) |
| Server response | [`server_emoji_error`](#server-response-server_emoji_error) |

### Client request: `load_server_emojis`

**Body**

```json
{
    "message":
    {
        "type": "load_server_emojis"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"load_server_emojis"`. |

No additional body fields.

### Client request: `load_server_emoji`

**Body**

```json
{
    "message":
    {
        "type": "load_server_emoji",
        "emoji_id": "0123456789abcdef0123456789abcdef"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"load_server_emoji"`. |
| `message.emoji_id` | string | Required | Stable 32-character lowercase hexadecimal emoji ID. |

### Client request: `upload_server_emoji`

**Body**

```json
{
    "message":
    {
        "type": "upload_server_emoji",
        "name": "wave",
        "base64": "data:image/png;base64,<emoji bytes>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"upload_server_emoji"`. |
| `message.name` | string | Required | Emoji name: 1–32 lowercase letters, digits or underscores (`[a-z0-9_]`). |
| `message.base64` | string | Required | Image data URL: PNG, JPEG or GIF. At most 128 × 128 pixels; PNG/JPEG at most 30000 bytes; GIF at most 256 KiB and 200 frames. |

### Client request: `delete_server_emoji`

**Body**

```json
{
    "message":
    {
        "type": "delete_server_emoji",
        "emoji_id": "0123456789abcdef0123456789abcdef"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"delete_server_emoji"`. |
| `message.emoji_id` | string | Required | Stable 32-character lowercase hexadecimal emoji ID. |

### Client request: `save_server_emojis`

**Body**

```json
{
    "message":
    {
        "type": "save_server_emojis"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"save_server_emojis"`. |

No additional body fields.

### Server response: `server_emoji_list`

**Body**

```json
{
    "message":
    {
        "type": "server_emoji_list",
        "emojis":
        [
            {
                "id": "0123456789abcdef0123456789abcdef",
                "name": "wave",
                "unicode": "👋"
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_emoji_list"`. |
| `message.emojis` | object[] | Required | Catalog entries. Image bytes are not included. |
| `message.emojis[].id` | string | Required | Stable 32-character lowercase hexadecimal ID. Catalog entries use `id`, not `emoji_id`. |
| `message.emojis[].name` | string | Required | Emoji name: 1–32 lowercase letters, digits or underscores (`[a-z0-9_]`). |
| `message.emojis[].unicode` | string | Conditional: text emoji | Unicode text for a text emoji. Image entries omit this member. |

### Server response: `server_emoji_image`

**Body**

```json
{
    "message":
    {
        "type": "server_emoji_image",
        "emoji_id": "0123456789abcdef0123456789abcdef",
        "name": "wave",
        "base64": "data:image/png;base64,<emoji bytes>"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_emoji_image"`. |
| `message.emoji_id` | string | Required | Stable 32-character lowercase hexadecimal emoji ID. |
| `message.name` | string | Required | Emoji name: 1–32 lowercase letters, digits or underscores (`[a-z0-9_]`). |
| `message.base64` | string | Required | Image data URL: PNG, JPEG or GIF. At most 128 × 128 pixels; PNG/JPEG at most 30000 bytes; GIF at most 256 KiB and 200 frames. |

### Server response: `server_emoji_added`

**Body**

```json
{
    "message":
    {
        "type": "server_emoji_added",
        "id": "0123456789abcdef0123456789abcdef",
        "name": "wave"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_emoji_added"`. |
| `message.id` | string | Required | New emoji ID. This body uses `id`, not `emoji_id`. |
| `message.name` | string | Required | Emoji name: 1–32 lowercase letters, digits or underscores (`[a-z0-9_]`). |

### Server response: `server_emoji_deleted`

**Body**

```json
{
    "message":
    {
        "type": "server_emoji_deleted",
        "emoji_id": "0123456789abcdef0123456789abcdef"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_emoji_deleted"`. |
| `message.emoji_id` | string | Required | Stable 32-character lowercase hexadecimal emoji ID. |

### Server response: `server_emoji_error`

**Body**

```json
{
    "message":
    {
        "type": "server_emoji_error",
        "operation": "load_server_emoji",
        "reason": "This emoji is no longer available.",
        "emoji_id": "0123456789abcdef0123456789abcdef"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"server_emoji_error"`. |
| `message.operation` | string | Required | Emoji request type associated with the error, such as `load_server_emoji`, `upload_server_emoji` or `delete_server_emoji`. |
| `message.reason` | string | Required | Readable error text; not a stable code enum. |
| `message.emoji_id` | string | Conditional: an emoji ID was supplied and is available | Stable 32-character lowercase hexadecimal emoji ID. |

**Upload error without an ID**

```json
{
    "message":
    {
        "type": "server_emoji_error",
        "operation": "upload_server_emoji",
        "reason": "Use one to 32 lowercase letters, digits or underscores for the name."
    }
}
```

**Collection storage limit**

```json
{
    "message":
    {
        "type": "server_emoji_error",
        "operation": "upload_server_emoji",
        "reason": "Emoji images have a combined 32 MiB encoded storage limit. Delete some images before uploading another."
    }
}
```

## Audio and media connection bodies

| Direction | Type |
| --- | --- |
| Client request | [`request_audio_websocket`](#client-request-request_audio_websocket) |
| Client request | [`request_video_websocket`](#client-request-request_video_websocket) |
| Client request | [`create_new_webrtc_datachannel_connection`](#client-request-create_new_webrtc_datachannel_connection) |
| Client request | [`sdp_answer`](#client-request-sdp_answer) |
| Client request | [`ice_candidate`](#client-request-ice_candidate) |
| Client request | [`microphone_usage`](#client-request-microphone_usage) |
| Server response | [`audio_websocket_session`](#server-response-audio_websocket_session) |
| Server response | [`video_websocket_session`](#server-response-video_websocket_session) |
| Server response | [`sdp_offer`](#server-response-sdp_offer) |
| Server response | [`ice_candidate`](#server-response-ice_candidate) |
| Server response | [`datachannel_cooldown`](#server-response-datachannel_cooldown) |
| Server response | [`audio_state_of_single_client`](#server-response-audio_state_of_single_client) |
| Server response | [`current_channel_active_microphone_usage`](#server-response-current_channel_active_microphone_usage) |

### Client request: `request_audio_websocket`

**Body**

```json
{
    "message":
    {
        "type": "request_audio_websocket"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"request_audio_websocket"`. |

No additional body fields.

### Client request: `request_video_websocket`

**Body**

```json
{
    "message":
    {
        "type": "request_video_websocket"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"request_video_websocket"`. |

No additional body fields.

### Client request: `create_new_webrtc_datachannel_connection`

**Body**

```json
{
    "message":
    {
        "type": "create_new_webrtc_datachannel_connection"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"create_new_webrtc_datachannel_connection"`. |

No additional body fields.

### Client request: `sdp_answer`

**Body**

```json
{
    "message":
    {
        "type": "sdp_answer",
        "value":
        {
            "type": "answer",
            "sdp": "v=0\r\n<SDP answer lines>\r\n"
        }
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"sdp_answer"`. |
| `message.value` | object | Required | SDP description as an actual JSON object, not an encoded JSON string. |
| `message.value.type` | string | Required | Nonempty SDP description type. The client sends `"answer"`; the current validator checks nonemptiness rather than this exact literal. |
| `message.value.sdp` | string | Required | Nonempty SDP text, including escaped line breaks inside the JSON string. |

### Client request: `ice_candidate`

**Body**

```json
{
    "message":
    {
        "type": "ice_candidate",
        "value":
        {
            "candidate": "candidate:1 1 UDP 2122260223 192.0.2.8 50000 typ host",
            "sdpMid": "0",
            "sdpMLineIndex": 0
        }
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"ice_candidate"`. |
| `message.value` | object | Required | Candidate object. All three members are required; no null end-of-candidates body is defined here. |
| `message.value.candidate` | string | Required | ICE candidate string. The request validator accepts an empty string. |
| `message.value.sdpMid` | string | Required | SDP media-section identifier. The request validator accepts an empty string. |
| `message.value.sdpMLineIndex` | number | Required | SDP media-line index as a JSON number. |

### Client request: `microphone_usage`

**Body**

```json
{
    "message":
    {
        "type": "microphone_usage",
        "value": 2
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"microphone_usage"`. |
| `message.value` | number | Required | `1` transmitting; `2` push-to-talk enabled; `3` microphone disabled, reception enabled; `4` all audio disabled. |

### Server response: `audio_websocket_session`

**Body**

```json
{
    "message":
    {
        "type": "audio_websocket_session",
        "client_id": 7,
        "auth_key": "0101010101010101010101010101010101010101010101010101010101010101",
        "ticket": "02020202020202020202020202020202"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"audio_websocket_session"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.auth_key` | string | Required | 64 hexadecimal characters encoding a 32-byte session authentication key. |
| `message.ticket` | string | Required | 32 hexadecimal characters encoding a 16-byte media-session ticket. |

### Server response: `video_websocket_session`

**Body**

```json
{
    "message":
    {
        "type": "video_websocket_session",
        "client_id": 7,
        "auth_key": "0101010101010101010101010101010101010101010101010101010101010101",
        "ticket": "02020202020202020202020202020202"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_websocket_session"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.auth_key` | string | Required | 64 hexadecimal characters encoding a 32-byte session authentication key. |
| `message.ticket` | string | Required | 32 hexadecimal characters encoding a 16-byte media-session ticket. |

### Server response: `sdp_offer`

**Body**

```json
{
    "message":
    {
        "type": "sdp_offer",
        "value": "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\n<SDP offer lines>\\r\\n\"}"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"sdp_offer"`. |
| `message.value` | string | Required | String containing serialized JSON with `type: "offer"` and `sdp: string`. Unlike `sdp_answer.value`, this member is not an object. See [Decoded SDP offer value](#decoded-sdp-offer-value). |

### Server response: `ice_candidate`

**Body**

```json
{
    "message":
    {
        "type": "ice_candidate",
        "value":
        {
            "candidate": "candidate:1 1 UDP 2122260223 192.0.2.8 50000 typ host",
            "sdpMid": "0",
            "sdpMLineIndex": 0
        }
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"ice_candidate"`. |
| `message.value` | object | Required | Server ICE candidate object. |
| `message.value.candidate` | string | Required | ICE candidate string. The request validator accepts an empty string. |
| `message.value.sdpMid` | string | Required | SDP media-section identifier. The request validator accepts an empty string. |
| `message.value.sdpMLineIndex` | number | Required | Current server serializer sets this numeric index to `0`. |

### Server response: `datachannel_cooldown`

**Body**

```json
{
    "message":
    {
        "type": "datachannel_cooldown",
        "seconds_left": 10
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"datachannel_cooldown"`. |
| `message.seconds_left` | number | Required | Remaining WebRTC cooldown in seconds. |

### Server response: `audio_state_of_single_client`

**Body**

```json
{
    "message":
    {
        "type": "audio_state_of_single_client",
        "client_id": 7,
        "value": 2
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"audio_state_of_single_client"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.value` | number | Required | `1` transmitting; `2` push-to-talk enabled; `3` microphone disabled, reception enabled; `4` all audio disabled. Active transmission can appear as state 2 outside the relevant channel. |

### Server response: `current_channel_active_microphone_usage`

Entries have no `username`, `public_key`, `group_ids`, `music_bot_encryption` or `music_bot_keys` fields.

**Body**

```json
{
    "message":
    {
        "type": "current_channel_active_microphone_usage",
        "clients":
        [
            {
                "client_id": 7,
                "audio_state": 1,
                "is_streaming_song": false,
                "is_streaming_video": false,
                "song_name": ""
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"current_channel_active_microphone_usage"`. |
| `message.clients` | object[] | Required | Only active-audio entries for the channel; an empty array means none are listed. |
| `message.clients[].client_id` | number | Required | Numeric client slot ID. |
| `message.clients[].audio_state` | number | Required | For every entry in this body, the value is `1` (active transmission). |
| `message.clients[].is_streaming_song` | boolean | Required | Whether this active client is streaming a song. |
| `message.clients[].is_streaming_video` | boolean | Required | Whether this active client is streaming video. |
| `message.clients[].song_name` | string | Required | Song display name; `""` when no song name is set. |

## Video stream bodies

| Direction | Type |
| --- | --- |
| Client request | [`video_stream_start`](#client-request-video_stream_start) |
| Client request | [`video_stream_stop`](#client-request-video_stream_stop) |
| Client request | [`video_stream_watch`](#client-request-video_stream_watch) |
| Client request | [`video_stream_allow_viewer`](#client-request-video_stream_allow_viewer) |
| Client request | [`video_stream_keyframe_request`](#client-request-video_stream_keyframe_request) |
| Server response | [`video_stream_state`](#server-response-video_stream_state) |
| Server response | [`video_stream_offer`](#server-response-video_stream_offer) |
| Server response | [`video_stream_viewer_request`](#server-response-video_stream_viewer_request) |
| Server response | [`video_stream_viewer_state`](#server-response-video_stream_viewer_state) |
| Server response | [`video_stream_keyframe_request`](#server-response-video_stream_keyframe_request) |
| Server response | [`video_stream_refused`](#server-response-video_stream_refused) |

### Client request: `video_stream_start`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_start",
        "source": "screen",
        "codec": "avc1.42E01E",
        "width": 1280,
        "height": 720,
        "fps": 30,
        "is_sound_included": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_start"`. |
| `message.source` | string | Required | Exactly `"screen"` or `"file"`. |
| `message.codec` | string | Required | Nonempty codec identifier shorter than 32 characters; letters, digits and dots only. |
| `message.width` | number | Required | Video width in pixels, from `16` through `4096`. |
| `message.height` | number | Required | Video height in pixels, from `16` through `4096`. |
| `message.fps` | number | Required | Frame rate, from `1` through `60` frames per second. |
| `message.is_sound_included` | boolean | Optional; defaults to false | Whether the stream includes sound. |

### Client request: `video_stream_stop`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_stop"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_stop"`. |

No additional body fields.

### Client request: `video_stream_watch`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_watch",
        "is_watching": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_watch"`. |
| `message.is_watching` | boolean | Required | True requests watching; false withdraws watching. |

### Client request: `video_stream_allow_viewer`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_allow_viewer",
        "client_id": 7,
        "is_allowed": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_allow_viewer"`. |
| `message.client_id` | number | Required | Client ID of the viewer. |
| `message.is_allowed` | boolean | Required | True grants viewing; false revokes it. |

### Client request: `video_stream_keyframe_request`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_keyframe_request"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_keyframe_request"`. |

No additional body fields.

### Server response: `video_stream_state`

Inactive bodies omit all six stream-description fields; they do not replace them with zero, false or null.

**Body**

```json
{
    "message":
    {
        "type": "video_stream_state",
        "channel_id": 3,
        "streamer_client_id": 7,
        "is_active": true,
        "reason": "started",
        "source": "screen",
        "codec": "avc1.42E01E",
        "width": 1280,
        "height": 720,
        "fps": 30,
        "is_sound_included": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_state"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.streamer_client_id` | number | Required | Client ID of the streamer. |
| `message.is_active` | boolean | Required | Whether the channel has an active video stream. |
| `message.reason` | string | Required | State reason. Current values: `started`, `stopped`, `restarted`, `disabled`, `channel_disabled`, `insufficient_permissions`, `streamer_left`, `streamer_transport_lost`, `moderator_stopped`. |
| `message.source` | string | Present only when is_active is true | Exactly `"screen"` or `"file"`. |
| `message.codec` | string | Present only when is_active is true | Nonempty codec identifier shorter than 32 characters; letters, digits and dots only. |
| `message.width` | number | Present only when is_active is true | Video width in pixels, from `16` through `4096`. |
| `message.height` | number | Present only when is_active is true | Video height in pixels, from `16` through `4096`. |
| `message.fps` | number | Present only when is_active is true | Frame rate, from `1` through `60` frames per second. |
| `message.is_sound_included` | boolean | Present only when is_active is true | Whether the stream includes sound. |

**Inactive stream**

```json
{
    "message":
    {
        "type": "video_stream_state",
        "channel_id": 3,
        "streamer_client_id": 7,
        "is_active": false,
        "reason": "stopped"
    }
}
```

### Server response: `video_stream_offer`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_offer",
        "streamer_client_id": 7,
        "source": "screen",
        "codec": "avc1.42E01E",
        "width": 1280,
        "height": 720,
        "fps": 30,
        "is_sound_included": true,
        "watching_client_ids":
        [8]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_offer"`. |
| `message.streamer_client_id` | number | Required | Client ID of the streamer. |
| `message.source` | string | Required | Exactly `"screen"` or `"file"`. |
| `message.codec` | string | Required | Nonempty codec identifier shorter than 32 characters; letters, digits and dots only. |
| `message.width` | number | Required | Video width in pixels, from `16` through `4096`. |
| `message.height` | number | Required | Video height in pixels, from `16` through `4096`. |
| `message.fps` | number | Required | Frame rate, from `1` through `60` frames per second. |
| `message.is_sound_included` | boolean | Required | Whether the stream includes sound. |
| `message.watching_client_ids` | number[] | Required | Client IDs currently recorded as watching; may be empty. |

### Server response: `video_stream_viewer_request`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_viewer_request",
        "client_id": 7,
        "username": "Alice"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_viewer_request"`. |
| `message.client_id` | number | Required | Client ID of the requesting viewer. |
| `message.username` | string | Required | Client display name. |

### Server response: `video_stream_viewer_state`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_viewer_state",
        "client_id": 7,
        "is_watching": true
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_viewer_state"`. |
| `message.client_id` | number | Required | Client ID of the viewer. |
| `message.is_watching` | boolean | Required | Whether this viewer is watching. |

### Server response: `video_stream_keyframe_request`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_keyframe_request"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_keyframe_request"`. |

No additional body fields.

### Server response: `video_stream_refused`

**Body**

```json
{
    "message":
    {
        "type": "video_stream_refused",
        "reason": "busy"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"video_stream_refused"`. |
| `message.reason` | string | Required | Exactly one of `not_allowed`, `disabled`, `channel_disabled`, `insufficient_permissions`, `busy`, `no_stream` or `revoked`. |

## Music and music-bot bodies

| Direction | Type |
| --- | --- |
| Client request | [`start_song_stream`](#client-request-start_song_stream) |
| Client request | [`stop_song_stream`](#client-request-stop_song_stream) |
| Client request | [`create_music_bot`](#client-request-create_music_bot) |
| Client request | [`delete_music_bot`](#client-request-delete_music_bot) |
| Client request | [`musicbot_get_song_list`](#client-request-musicbot_get_song_list) |
| Client request | [`remove_song_from_music_bot`](#client-request-remove_song_from_music_bot) |
| Client request | [`musicbot_playback`](#client-request-musicbot_playback) |
| Client request | [`request_music_bot_key`](#client-request-request_music_bot_key) |
| Server response | [`start_song_stream`](#server-response-start_song_stream) |
| Server response | [`stop_song_stream`](#server-response-stop_song_stream) |
| Server response | [`music_bot_song_list`](#server-response-music_bot_song_list) |
| Server response | [`music_bot_key`](#server-response-music_bot_key) |

### Client request: `start_song_stream`

**Body**

```json
{
    "message":
    {
        "type": "start_song_stream",
        "song_name": "Example track"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"start_song_stream"`. |
| `message.song_name` | string | Required | Nonempty song display name, at most `SONG_NAME_MAX_LENGTH - 1` (511) UTF-8 characters. |

### Client request: `stop_song_stream`

**Body**

```json
{
    "message":
    {
        "type": "stop_song_stream"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"stop_song_stream"`. |

No additional body fields.

### Client request: `create_music_bot`

**Body**

```json
{
    "message":
    {
        "type": "create_music_bot",
        "channel_id": 3,
        "music_bot_username": "Radio"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"create_music_bot"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.music_bot_username` | string | Required | Nonempty music-bot display name shorter than `USERNAME_MAX_LENGTH` (100). |

### Client request: `delete_music_bot`

This body identifies the channel, not a `bot_id` or `musicbot_id`.

**Body**

```json
{
    "message":
    {
        "type": "delete_music_bot",
        "channel_id": 3
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"delete_music_bot"`. |
| `message.channel_id` | number | Required | Numeric channel ID. |

### Client request: `musicbot_get_song_list`

**Body**

```json
{
    "message":
    {
        "type": "musicbot_get_song_list",
        "musicbot_id": 9
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"musicbot_get_song_list"`. |
| `message.musicbot_id` | number | Required | Music bot client ID. This spelling has no underscore between music and bot. |

### Client request: `remove_song_from_music_bot`

**Body**

```json
{
    "message":
    {
        "type": "remove_song_from_music_bot",
        "musicbot_id": 9,
        "song_id": 0
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"remove_song_from_music_bot"`. |
| `message.musicbot_id` | number | Required | Music bot client ID. This spelling has no underscore between music and bot. |
| `message.song_id` | number | Required | Numeric song queue index from the bot's song list. |

### Client request: `musicbot_playback`

**Body**

```json
{
    "message":
    {
        "type": "musicbot_playback",
        "bot_id": 9,
        "action": "play"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"musicbot_playback"`. |
| `message.bot_id` | number | Required | Music bot client ID; this request uses `bot_id`. |
| `message.action` | string | Required | Exactly `"play"`, `"pause"` or `"next"`. |

### Client request: `request_music_bot_key`

**Body**

```json
{
    "message":
    {
        "type": "request_music_bot_key",
        "client_id": 9
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"request_music_bot_key"`. |
| `message.client_id` | number | Required | Client ID of the music bot. |

### Server response: `start_song_stream`

**Body**

```json
{
    "message":
    {
        "type": "start_song_stream",
        "client_id": 7,
        "song_name": "Example track"
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"start_song_stream"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |
| `message.song_name` | string | Required | Song display name; `""` when no song name is set. |

### Server response: `stop_song_stream`

**Body**

```json
{
    "message":
    {
        "type": "stop_song_stream",
        "client_id": 7
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"stop_song_stream"`. |
| `message.client_id` | number | Required | Numeric client slot ID. |

### Server response: `music_bot_song_list`

**Body**

```json
{
    "message":
    {
        "type": "music_bot_song_list",
        "bot_id": 9,
        "is_paused": false,
        "songs":
        [
            {
                "name": "Example track",
                "id": 0,
                "duration_seconds": 180
            }
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"music_bot_song_list"`. |
| `message.bot_id` | number | Required | Music bot client ID. |
| `message.is_paused` | boolean | Required | Whether bot playback is paused. |
| `message.songs` | object[] | Required | Songs in the current queue; may be empty. |
| `message.songs[].name` | string | Required | Song display name. |
| `message.songs[].id` | number | Required | Zero-based queue index; not a permanent song identifier. |
| `message.songs[].duration_seconds` | number | Required | Track duration in seconds. |

### Server response: `music_bot_key`

The encryption marker is conditional on key availability; the key array is separately conditional on recipient eligibility.

**Body**

```json
{
    "message":
    {
        "type": "music_bot_key",
        "client_id": 9,
        "channel_id": 3,
        "music_bot_encryption": 1,
        "music_bot_keys":
        [
            "0101010101010101010101010101010101010101010101010101010101010101",
            "0202020202020202020202020202020202020202020202020202020202020202",
            "0303030303030303030303030303030303030303030303030303030303030303",
            "0404040404040404040404040404040404040404040404040404040404040404"
        ]
    }
}
```

**Fields**

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message.type` | string | Required | Literal `"music_bot_key"`. |
| `message.client_id` | number | Required | Client ID of the music bot. |
| `message.channel_id` | number | Required | Numeric channel ID. |
| `message.music_bot_encryption` | number | Conditional: bot has audio keys | Encryption format version. The current value is exactly `1`. |
| `message.music_bot_keys` | string[] | Conditional: bot has keys and recipient is active in its channel | Exactly four 64-character lowercase hexadecimal strings, each encoding a 32-byte audio key. Present only when the bot has keys and the recipient is non-idle in the bot's channel. |

**No key material included**

```json
{
    "message":
    {
        "type": "music_bot_key",
        "client_id": 9,
        "channel_id": 3
    }
}
```

## JSON inside content fields

These objects are payload shapes, not additional top-level server message types. A field described as serialized JSON has JSON type `string` on the outer body. Parsing an encrypted envelope exposes its field names and ciphertext, not the plaintext protected by that ciphertext.

**The decrypted direct-message, formatted-text, quote, receipt, reaction, key-array and file-content examples below are client-side plaintext. They are not readable server request/response bodies.** The encrypted envelope examples still show ciphertext. The final SDP-offer example is separate media-signaling metadata and is readable by the server.

### Direct encrypted envelope

JSON parsed from the `value` string of a direct or offline text message. This envelope is visible to the server, but both the usable content keys and the plaintext content remain encrypted. Parsing this JSON does not decrypt either field.

```json
{
    "message_keys": "<RSA-encrypted keys>",
    "message_value": "{\"iv\":\"<base64 IV>\",\"data\":\"<base64 ciphertext>\"}"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message_keys` | string | Required | Serialized four-key array encrypted with the recipient's RSA public key. The recipient's private key is needed to recover the content keys; the server does not receive those keys in plaintext. |
| `message_value` | string | Required | Serialized encrypted text/control envelope containing `iv` and `data`. The server can see the ciphertext string but cannot read its plaintext content. |

### Encrypted text or control envelope

Decoded encrypted text envelope used as channel `value` or direct `message_value`. It has no transport-level `tag` member.

```json
{
    "iv": "<base64 IV>",
    "data": "<base64 ciphertext>"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `iv` | string | Required | Base64-encoded random IV used by the content-encryption format. |
| `data` | string | Required | Base64-encoded encrypted content. |

### Decrypted direct text control body

**Client-side plaintext; not visible to the server.** This is the object before content encryption or after recipient decryption. Its `value` contains the serialized formatted-text body. The identically named outer `direct_chat_message` body contains ciphertext instead.

```json
{
    "type": "direct_chat_message",
    "value": "{\"reaction_id\":\"00112233445566778899aabbccddeeff\",\"reply_to\":null,\"font\":\"Arial\",\"font_size\":12,\"font_color\":\"#000000\",\"value\":\"Hello everyone.\"}"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `type` | string | Required | Literal `"direct_chat_message"`. |
| `value` | string | Required | Serialized formatted-text JSON; the reaction variant uses the reaction-control JSON instead. |

### Formatted text body

**Client-side plaintext.** In a direct message, all fields shown below are inside the end-to-end encrypted content and are not visible to the server. The same formatted-text shape is also used as decrypted channel text. There is no `type` member in this object.

```json
{
    "reaction_id": "00112233445566778899aabbccddeeff",
    "reply_to": null,
    "font": "Arial",
    "font_size": 12,
    "font_color": "#000000",
    "value": "Hello everyone."
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `reaction_id` | string | Required | Client-generated 32-character lowercase hexadecimal identifier for edits, reactions and reply references. It is not the numeric server message ID. |
| `reply_to` | object or null | Required | Quoted-message reference, or `null` for a message with no quote. The object schema is expanded in the quoted-reply section. |
| `font` | string | Required | Selected font family string. |
| `font_size` | number | Required | Font size as a JSON number, not a quoted number. |
| `font_color` | string | Required | Selected font color string. |
| `value` | string | Required | Actual chat text. |

### Formatted text with a quoted reply

The non-null `reply_to` variant of the same formatted-text body. In a direct message, the entire quote object, including its author fields and quoted text, remains inside the encrypted content and is not visible to the server.

```json
{
    "reaction_id": "00112233445566778899aabbccddeeff",
    "reply_to":
    {
        "message_id": "ffeeddccbbaa99887766554433221100",
        "author_public_key": "<RSA public key>",
        "author_name": "Bob",
        "text": "Original message."
    },
    "font": "Arial",
    "font_size": 12,
    "font_color": "#000000",
    "value": "Hello everyone."
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `reaction_id` | string | Required | Client-generated 32-character lowercase hexadecimal identifier for edits, reactions and reply references. It is not the numeric server message ID. |
| `reply_to` | object | Required | Quoted-message reference object; all four members belong to this object. |
| `reply_to.message_id` | string | Required | 32-character lowercase hexadecimal reaction/reply ID of the quoted message, not a numeric server message ID. |
| `reply_to.author_public_key` | string | Required | Nonempty author public key string, at most 16384 characters in the reply normalizer. |
| `reply_to.author_name` | string | Required | Quoted author display name, truncated to 128 characters by normalization. |
| `reply_to.text` | string | Required | Quoted text excerpt, truncated to 320 characters by normalization. |
| `font` | string | Required | Selected font family string. |
| `font_size` | number | Required | Font size as a JSON number, not a quoted number. |
| `font_color` | string | Required | Selected font color string. |
| `value` | string | Required | Actual chat text. |

### Decrypted seen receipt

**Client-side plaintext; not visible to the server.** Both the inner `type: "message_seen"` and its `value` are inside the encrypted direct-message content.

```json
{
    "type": "message_seen",
    "value": "900"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `type` | string | Required | Literal `"message_seen"`. |
| `value` | string | Required | Server chat message ID encoded as a decimal string. Unlike outer message IDs, this value is a string. |

### Decrypted channel-key control body

**Client-side plaintext; not visible to the server.** This control object is inside encrypted direct-message content. Its `value` is itself a serialized array of exactly four key objects; these are not plaintext keys in the outer server body.

```json
{
    "type": "channel_keys_from_maintainer",
    "channel_id": 3,
    "maintainer_generation": 12,
    "value": "[{\"key_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"iv_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]},{\"key_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"iv_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]},{\"key_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"iv_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]},{\"key_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31],\"iv_bytes\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]}]"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `type` | string | Required | Literal `"channel_keys_from_maintainer"`. |
| `channel_id` | number | Required | Numeric channel ID. |
| `maintainer_generation` | number | Required | Generation associated with the announced channel maintainer. |
| `value` | string | Required | String containing the four-key array shown in the next section. |

### Four-key array

**Client-side plaintext; not visible to the server.** This is the decoded array from a decrypted key-control `value` or from RSA-decrypted `message_keys`. It contains exactly four objects. The array is not wrapped in a `message` object and is not supplied to the server as plaintext.

```json
[
    {
        "key_bytes":
        [
            0,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            9,
            10,
            11,
            12,
            13,
            14,
            15,
            16,
            17,
            18,
            19,
            20,
            21,
            22,
            23,
            24,
            25,
            26,
            27,
            28,
            29,
            30,
            31
        ],
        "iv_bytes":
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
    },
    {
        "key_bytes":
        [
            0,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            9,
            10,
            11,
            12,
            13,
            14,
            15,
            16,
            17,
            18,
            19,
            20,
            21,
            22,
            23,
            24,
            25,
            26,
            27,
            28,
            29,
            30,
            31
        ],
        "iv_bytes":
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
    },
    {
        "key_bytes":
        [
            0,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            9,
            10,
            11,
            12,
            13,
            14,
            15,
            16,
            17,
            18,
            19,
            20,
            21,
            22,
            23,
            24,
            25,
            26,
            27,
            28,
            29,
            30,
            31
        ],
        "iv_bytes":
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
    },
    {
        "key_bytes":
        [
            0,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            9,
            10,
            11,
            12,
            13,
            14,
            15,
            16,
            17,
            18,
            19,
            20,
            21,
            22,
            23,
            24,
            25,
            26,
            27,
            28,
            29,
            30,
            31
        ],
        "iv_bytes":
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]
    }
]
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `[].key_bytes` | number[] | Required | Exactly 32 integer bytes, each from `0` through `255`. |
| `[].iv_bytes` | number[] | Required | Exactly 16 integer bytes, each from `0` through `255`. |

### Edit control body

**Client-side plaintext, encrypted before sending.** This body replaces the formatted-text payload when the author edits an existing message. For a direct edit it is serialized into the inner `direct_chat_message.value`, protected by fresh message keys encrypted for the recipient. For a channel edit it is encrypted with the current channel keys. Every field below, including `new_message_value`, is inside content encryption and is not readable by the server.

```json
{
    "type": "chat_message_edit",
    "message_id": "00112233445566778899aabbccddeeff",
    "author_public_key": "<RSA public key>",
    "new_message_value": "Corrected message"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `type` | string | Required | Literal `"chat_message_edit"`, inside encrypted chat content. |
| `message_id` | string | Required | Original message's 32-character lowercase hexadecimal `reaction_id`, not its numeric server message ID. |
| `author_public_key` | string | Required | Original author's public key. Must match the sender's public key and the author recorded for the target message. |
| `new_message_value` | string | Required | Replacement text with at least one non-whitespace character. Rendered as text, including literal HTML characters and newlines. |

The `message_id` is copied unchanged from the original message's `reaction_id`; a new random ID would not identify that message. The outer body is an ordinary `direct_chat_message` or `channel_chat_message`, with its normal recipient and delivery IDs. Its `value` contains ciphertext. The receiving client matches the target within that conversation and rejects an edit from another author, an earlier connection or an older edit sequence. Channel edits require valid channel keys; there is no plaintext fallback.

### Reaction control body

Reaction JSON carried as encrypted chat content. In a direct message, every field shown here, including the emoji and target-message reference, is encrypted and not visible to the server. It has a `type` field and replaces the ordinary formatted-text payload for this variant.

```json
{
    "type": "chat_reaction",
    "message_id": "00112233445566778899aabbccddeeff",
    "author_public_key": "<RSA public key>",
    "emoji": "server:0123456789abcdef0123456789abcdef",
    "emoji_name": "wave",
    "active": true
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `type` | string | Required | Literal `"chat_reaction"`. |
| `message_id` | string | Required | Target message's 32-character lowercase hexadecimal reaction ID, not its numeric server message ID. |
| `author_public_key` | string | Required | Public key of the target message's author, not the reacting user. |
| `emoji` | string | Required | Server emoji reference: `server:` followed by a 32-character lowercase hexadecimal catalog ID. |
| `emoji_name` | string | Required | Emoji name, 1–32 lowercase letters, digits or underscores. |
| `active` | boolean | Required | True adds the reaction; false removes it. |

### Authenticated file envelope

Current clients serialize this object into the encrypted file-header string and the assembled file-body string. A channel file uses it directly. A direct file stores the serialized object in its outer envelope's `message_value`. The server can see the nonce, ciphertext and tag; it does not receive the file keys or plaintext.

```json
{
    "v": 2,
    "iv": "<base64 of 16 fresh random bytes>",
    "data": "<base64 ciphertext>",
    "tag": "<base64 of 32 HMAC-SHA256 bytes>"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `v` | number | Required | Literal integer `2`. |
| `iv` | string | Required | Fresh 16-byte nonce encoded as 24 Base64 characters, including padding. Headers and bodies have separate nonces. |
| `data` | string | Required | Four-layer AES-256-CTR ciphertext of UTF-8 JSON with zero padding: at least 1024 bytes for a header and 4096 bytes for a body. |
| `tag` | string | Required | 32-byte HMAC-SHA256 encoded as 44 Base64 characters. Verified before decryption. |

The v2 object contains exactly these four members. [File encryption](encryption.md#file-encryption) specifies the derivation and authenticated bytes. Updated clients also read legacy bare Base64 file ciphertext, but never generate it. Older receivers need an update to read v2 files; the server forwards the envelope without translating it.

### File header

**Client-side plaintext.** This is the decrypted JSON inside a chat file's encrypted `file_header`. For a direct file, the server cannot read any of these three fields. It contains file metadata and has no `data` member; the outer encrypted upload length remains server-visible separately.

```json
{
    "name": "notes.txt",
    "size": 4,
    "mime": "text/plain"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `name` | string | Required | Original filename. |
| `size` | number | Required | Original unencrypted file size in bytes. |
| `mime` | string | Required | File MIME type string. |

### File body

**Client-side plaintext.** This is the decrypted JSON for a complete chat file attachment. For a direct file, its name, original size, MIME type and file bytes are all inside encrypted content and are not visible to the server.

```json
{
    "name": "notes.txt",
    "size": 4,
    "mime": "text/plain",
    "data": "VGVzdA=="
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `name` | string | Required | Original filename. |
| `size` | number | Required | Original unencrypted file size in bytes. |
| `mime` | string | Required | File MIME type string. |
| `data` | string | Required | Base64-encoded raw file bytes. |

### Direct file encrypted envelope

JSON parsed from a direct-file envelope used for an encrypted header or complete file body. The server can see this envelope's ciphertext strings, but cannot read the decrypted header or file content.

```json
{
    "message_keys": "<RSA-encrypted keys>",
    "message_value": "<serialized authenticated file envelope>"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `message_keys` | string | Required | Serialized four-key array encrypted with the recipient's RSA public key. The plaintext content keys are not available to the server. |
| `message_value` | string | Required | Serialized v2 authenticated file envelope; its plaintext is not visible to the server. Legacy senders may supply bare Base64 ciphertext. |

Channel file bodies and channel file headers use the serialized v2 authenticated envelope without the direct-file `message_keys` wrapper. Decrypted picture content is an image representation rather than the JSON file-header/file-body objects above.

### Decoded SDP offer value

JSON contained in the string `sdp_offer.message.value`.

```json
{
    "type": "offer",
    "sdp": "v=0\r\n<SDP offer lines>\r\n"
}
```

| Field | JSON type | Presence | Body value |
| --- | --- | --- | --- |
| `type` | string | Required | Literal `"offer"`. |
| `sdp` | string | Required | SDP description text. |

## Compatibility body names

| Name | Body status in the current source |
| --- | --- |
| `channel_chat_picture` | A dormant server serializer exists; its complete body is documented above. |
| `server_info_broadcast` | Client compatibility handler name; no current server serializer emits a body of this type. Current public info uses `server_info`. |
| `client_role_add` | Client compatibility handler name; no current server serializer emits a body of this type. Current server-group membership uses `group_add_to_client`. |
| `direct_chat_picture_decrypted`, `direct_chat_file_decrypted` | Local worker result names, not server response body types. |
| `some_json`, `file_header_decrypted`, `decrypted_content` | Locally derived client fields, absent from the corresponding server wire bodies. |

## Implementation sources

The body names, field types and conditional members above were checked against the current dispatcher, request validators, response serializers and client content constructors.

- [Request type dispatch](../server-source-code/main/base.c)
- [Common application wrapper](../server-source-code/main/msg/server/server_msg_delivery.c)
- [Transport encryption](../server-source-code/main/crypto/crypto_metadata.c)
- [Connection request bodies](../server-source-code/main/msg/client/client_msg_connection.c)
- [Connection response bodies](../server-source-code/main/msg/server/server_msg_connection.c)
- [Channel request bodies](../server-source-code/main/msg/client/client_msg_channel.c)
- [Channel response bodies](../server-source-code/main/msg/server/server_msg_channel.c)
- [Chat request bodies](../server-source-code/main/msg/client/client_msg_chat.c)
- [Chat response bodies](../server-source-code/main/msg/server/server_msg_chat.c)
- [Attachments request bodies](../server-source-code/main/msg/client/client_msg_attachments.c)
- [Attachments response bodies](../server-source-code/main/msg/server/server_msg_attachments.c)
- [Profile request bodies](../server-source-code/main/msg/client/client_msg_profile.c)
- [Profile response bodies](../server-source-code/main/msg/server/server_msg_profile.c)
- [Administration request bodies](../server-source-code/main/msg/client/client_msg_administration.c)
- [Administration response bodies](../server-source-code/main/msg/server/server_msg_administration.c)
- [Moderation request bodies](../server-source-code/main/msg/client/client_msg_moderation.c)
- [Moderation response bodies](../server-source-code/main/msg/server/server_msg_moderation.c)
- [Server Settings request bodies](../server-source-code/main/msg/client/client_msg_server_settings.c)
- [Server Settings response bodies](../server-source-code/main/msg/server/server_msg_server_settings.c)
- [Emojis request bodies](../server-source-code/main/msg/client/client_msg_emojis.c)
- [Emojis response bodies](../server-source-code/main/msg/server/server_msg_emojis.c)
- [Audio request bodies](../server-source-code/main/msg/client/client_msg_audio.c)
- [Audio response bodies](../server-source-code/main/msg/server/server_msg_audio.c)
- [Video request bodies](../server-source-code/main/msg/client/client_msg_video.c)
- [Video response bodies](../server-source-code/main/msg/server/server_msg_video.c)
- [Musicbot request bodies](../server-source-code/main/msg/client/client_msg_musicbot.c)
- [Musicbot response bodies](../server-source-code/main/msg/server/server_msg_musicbot.c)
- [Server permissions and grants](../server-source-code/main/core/permissions.c)
- [Channel permission definitions](../server-source-code/main/core/channel_groups.c)
- [Emoji catalog and image shapes](../server-source-code/main/core/emojis.c)
- [URL preview fields](../server-source-code/main/httpclient/url_resolver.c)
- [Audio WebSocket session](../server-source-code/main/audio_websocket.c)
- [Video WebSocket session](../server-source-code/main/video_websocket.c)
- [SDP offer string](../server-source-code/main/audio_webrtc.c)
- [Content bodies](../client-source-code/client-development/browser/main/scripts/app/workers/workers.js)
- [Text and key encryption](../client-source-code/client-development/browser/main/scripts/app/crypto/keys.js)
- [Formatted text](../client-source-code/client-development/browser/main/scripts/app/chat/chat.js)
- [Quoted replies](../client-source-code/client-development/browser/main/scripts/app/chat/chat-replies.js)
- [Reactions](../client-source-code/client-development/browser/main/scripts/app/chat/chat-reactions.js)
- [File headers and file bodies](../client-source-code/client-development/browser/main/scripts/app/chat/chat-files.js)
