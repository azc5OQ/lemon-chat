# Permission reference

This reference contains the exact permission names, powers, protection values and saved formats used by Lemon Chat. Read [Permissions](permissions.md) first when you want to understand how groups, Enable, Disable, power, Negate, Skip and Grant work together.

## Contents

- [Saved values](#saved-values)
- [Server action permissions](#complete-server-action-list)
- [Grant, Negate and Skip fields](#complete-grant-and-flag-scope)
- [Server powers and protection](#powers-and-protection)
- [Channel action permissions](#complete-channel-action-list)
- [Channel powers and protection](#channel-power-fields)
- [Saved channel groups](#saved-channel-groups)
- [Source files](#where-the-code-lives)

## Saved values

Each server group is an entry in the `groups` array in `server_settings.json`. Its `permissions` object stores actions as `grant` or `deny`. Powers are whole numbers from 0 to 1000.

Simple view offers named values: None (0), Low (25), Normal (50), High (75), Very high (100) and Admin (1000). Advanced view accepts any whole number in the range. Switching views keeps custom values. The settings file always stores the number.

For example, this group can kick a user whose kick protection is 50 or less. It does not allow banning. Its own kick protection is 40. Other permission fields are left out to keep the example short.

```json
{
    "id": 1,
    "name": "Moderators",
    "group_type": "moderator",
    "is_default_guest": false,
    "has_icon": false,
    "icon_id": 0,
    "permissions":
    {
        "kick": "grant",
        "ban": "deny",
        "kick_power": 50,
        "kick_protection": 40
    }
}
```

`group_type` records the preset chosen when the group was created. The saved permission values decide what the group can do after edits. Only one group has `is_default_guest: true`.

Saved users keep their server groups in `identities[].group_ids`. Manage these in the right panel of Server groups, including for offline users. Removing a membership does not delete the identity. Guest is not saved alongside other groups; the server adds it when no other group remains.

### Older settings files

The server can still read older files with `default` or missing actions. The server group editor shows the old default for each action, listed in the table below. The channel group editor shows missing actions as Disable.

Opening the editor does not change permissions. Saving permission edits sends an explicit Enable or Disable for every displayed action. Until edited, old server values keep their earlier combination rule: an explicit Disable overrides an old default, while an explicit Enable overrides Disable.

## Available permissions

The complete permission catalog has the following scopes. These are the exact field names used inside stored and transmitted `permissions` objects, not just display labels from the editor.

| Permission scope | Complete count | Values |
| --- | --- | --- |
| Server action permissions | 53 | `grant`, `deny` or the legacy/default value `default`. The editor writes explicit Enable/Disable choices. |
| Server powers and protection values | 15 | Whole numbers from 0 through 1000. |
| Channel action permissions | 11 | A supported subset of the server action names. Stored output uses `grant` or `deny`; omitted or `default` input becomes `deny`. |
| Channel powers and protection values | 10 | Whole numbers from 0 through 1000, using the channel-specific names listed below. Omitted input becomes 0. |

`grants` and `flags` are additional objects that refer to these names. They are not additional action permissions. The four `required_*_power` fields belong to a channel's requirements, not to a group's permission catalog.

For the complete JSON bodies containing these fields, see [`set_group_permissions`](messages.md#client-request-set_group_permissions), [`group_list`](messages.md#server-response-group_list), [`group_permissions_changed`](messages.md#server-response-group_permissions_changed) and [`edit_channel_group`](messages.md#client-request-edit_channel_group).

### Complete server action list

Every action supported by a server-group permission object is listed here. Channel groups accept only the [11 actions in their own list](#complete-channel-action-list); a server permission that affects channels is not automatically a channel-group field.

**Old default** describes older settings files where the action is missing or saved as `default`. Yes means allowed; No means not allowed. This is not a third choice in the editor. New groups use their preset's explicit values.

| Permission | What it allows | Old default |
| --- | --- | --- |
| `create_channel` | Create a permanent channel. | No |
| `create_temp_channel` | Create one temporary channel when permanent channel creation is not allowed. It is removed when its owner leaves or disconnects. Changing an existing channel to temporary has separate checks. | Yes |
| `kick` | Disconnect another user, if kick power meets their kick protection. | No |
| `ban` | Ban another user, if ban power meets their ban protection. | No |
| `send_offline_messages` | Send encrypted messages to offline users. The server must also enable offline messages. | No |
| `receive_offline_messages` | Receive messages sent while offline. The sender must be allowed to send them. | No |
| `message_idle_clients` | Send private messages to connected users in idle mode. The server must also allow this. | Yes |
| `call_idle_clients` | Send call notifications to users in idle mode. The server must also enable idle mode. | Yes |
| `view_stored_users` | View the saved contact list for offline conversations. The server must also enable the list. | No |
| `view_group_permissions` | Read group permission values. | No |
| `view_group_members` | View saved users and their server group memberships in Server groups. | No |
| `create_groups` | Create a server group whose permissions and powers you are allowed to give. | No |
| `edit_groups` | Rename a server group or select its icon, if group protection allows it. | No |
| `delete_groups` | Delete a server group, if the group management checks allow it. | No |
| `assign_groups` | Add a server group to a connected user or saved identity. | No |
| `remove_groups` | Remove a server group from a connected user or saved identity. | No |
| `edit_group_permissions` | Change a server group's permissions and powers, if the editing checks allow it. | No |
| `upload_icons` | Upload server icons. Selecting a group's icon also needs `edit_groups`. | No |
| `view_client_connection_time` | View how long another user has been connected. Does not reveal their IP address or identity. | Yes |
| `view_client_details` | View identity, country and last activity. An IP address also needs `view_ip_addresses`. | No |
| `view_server_logs` | Read the administration log. Does not allow clearing it. | No |
| `view_server_info` | View the public server information the admin has chosen to share. | Yes |
| `delete_icons` | Delete shared server icons. Does not allow uploading them. | No |
| `upload_emojis` | Add server reaction images. PNG, JPEG, static WebP and supported GIF images can be selected. | No |
| `delete_emojis` | Remove emojis from the server's selection list. | No |
| `view_ip_addresses` | View IP addresses in client details and the ban list. Viewing those lists needs its own permission. | No |
| `view_bans` | Read the ban list. Does not allow adding bans. | No |
| `remove_bans` | Remove bans. Does not allow adding them. | No |
| `move_clients` | Move users between channels. Move power must meet their protection, and the move must be allowed in both channels. | No |
| `kick_from_channel` | Move a user to root without disconnecting them. The relevant power must meet their protection. | No |
| `mute_clients` | Temporarily stop a user's microphone and MP3 audio in a channel, if moderation power meets their protection. | No |
| `stop_client_streams` | Stop a screen share or video stream, if moderation power meets the user's protection. | No |
| `edit_channel_name` | Change a channel name. | No |
| `edit_channel_description` | Change a channel description. | No |
| `edit_channel_password` | Set, replace or remove a channel password. | No |
| `edit_channel_powers` | Change the required talk, message, picture and streaming powers. | No |
| `edit_channel_options` | Change audio, video, user limits, channel type and the selected channel icon. Changing type has extra checks. | No |
| `delete_channel` | Delete a channel. Deleting its child channels must also be allowed. | No |
| `assign_channel_groups` | Assign or remove a channel group, if the management checks allow it. | No |
| `send_direct_pictures` | Send pictures to individual users. | Yes |
| `send_direct_files` | Send files to individual users. | Yes |
| `send_channel_pictures` | Send channel pictures. The channel's picture power requirement also applies. | Yes |
| `send_channel_files` | Send channel files. | Yes |
| `create_music_bots` | Add a music bot when the server allows bot audio. | No |
| `delete_music_bots` | Remove music bots and their uploaded songs. | No |
| `upload_music_tracks` | Add MP3 files to a music bot. | No |
| `delete_music_tracks` | Remove songs from a music bot. | No |
| `control_music_playback` | Play, pause and select the next song. Does not allow uploading or deleting songs. | No |
| `use_reserved_slots` | Connect using a place reserved in server settings. Does not increase the total user limit. | No |
| `create_channel_groups` | Create channel group definitions within your permissions. Does not allow assigning them. | No |
| `edit_channel_groups` | Change channel group names, icons and permissions within your management power. | No |
| `delete_channel_groups` | Delete custom channel groups and their assignments within your management power. | No |
| `bypass_channel_password` | Join a channel without its password. The user limit still applies. | No |

Group names and icons remain visible beside users. Connected users' group IDs are also included in client updates. `view_group_members` controls access to the management list, including saved users; it does not hide the groups of online users.

`view_stored_users` allows the contact list for offline conversations. `view_group_members` allows the membership management list. Neither includes the other.

Viewing permissions does not allow editing them. The editor needs both viewing and editing rights. Viewing members does not allow adding or removing them.

### Legacy permission name

`manage_icons` is an accepted legacy input alias, not a 54th current action. Its value can be `grant`, `deny` or `default`. It supplies `upload_icons` and `delete_icons` only when the respective explicit field is absent. Current output contains the two separate names. Channel groups do not accept `manage_icons`, and it is not a supported key in `grants` or `flags`.

### Complete Grant and flag scope

For example, `permissions.grants.kick` is the editing threshold for the `kick` action, while `permissions.flags.kick.negate` is that action's Negate flag. The action itself is still stored in `permissions.kick`.

| Member | Server-group scope | Channel-group scope |
| --- | --- | --- |
| `permissions.grants.<name>` | Every one of the 53 server action names and 15 server power names. | Every one of the 11 channel action names and 10 channel power names. |
| `permissions.flags.<name>.negate` | Every server action or power. An action must have an explicit `grant` or `deny` value; `default` cannot carry flags. | Every supported channel action or power. |
| `permissions.flags.<name>.skip` | Only the 21 server fields in the table below. | No channel field supports Skip. |

Grant values are whole numbers from 0 through 1000 and default to 1000 when absent. Negate and Skip are booleans and default to false when absent. Only active flags are emitted in output. Unsupported names are rejected, including an unsupported `skip` member set to false.

This is the complete list of server fields that support Skip and the channel field whose contribution it excludes:

| Server field supporting Skip | Matching channel field |
| --- | --- |
| `kick_from_channel` | `kick_from_channel` |
| `mute_clients` | `mute_clients` |
| `stop_client_streams` | `stop_client_streams` |
| `edit_channel_name` | `edit_channel_name` |
| `edit_channel_description` | `edit_channel_description` |
| `edit_channel_password` | `edit_channel_password` |
| `edit_channel_powers` | `edit_channel_powers` |
| `edit_channel_options` | `edit_channel_options` |
| `delete_channel` | `delete_channel` |
| `assign_channel_groups` | `assign_channel_groups` |
| `bypass_channel_password` | `bypass_channel_password` |
| `group_management_power` | `channel_group_assignment_power` |
| `group_protection` | `channel_group_protection` |
| `talk_power` | `talk_power` |
| `channel_message_power` | `channel_message_power` |
| `channel_picture_power` | `channel_picture_power` |
| `stream_power` | `stream_power` |
| `move_power` | `channel_kick_power` |
| `move_protection` | `channel_kick_protection` |
| `moderation_power` | `channel_moderation_power` |
| `moderation_protection` | `channel_moderation_protection` |

Server `kick_power`, `kick_protection`, `ban_power`, `ban_protection` and `permission_edit_power` have no channel counterpart and do not support Skip. `move_clients` is also a server-only action; the channel kick fields do not grant arbitrary moves between channels.

## Saved users and messages

Saved identities keep group membership when users reconnect. Usernames can change, so offline messages use an **identity hash**: a value calculated from the identity that identifies the recipient. Registration and admin assigned display names are no longer used.

Viewing saved users, sending offline messages and receiving offline messages are separate permissions. Permission to view the list does not allow sending or receiving messages.

To send an encrypted message to an offline user, the sender needs that user's public key. The server includes it in the saved contact list only when offline messages are enabled, the sender can send them, and the recipient can receive them. The server checks the recipient's permission again when storing the message and when the recipient reconnects.

A user in **idle mode** is still connected. Messages and call notifications to idle users have separate permissions. New Guest groups disable both. Normal, Moderator and Admin presets allow both. Older settings without explicit choices use Yes for both, as shown in the table above.

When groups are disabled, ordinary users cannot view saved users or send and receive offline messages. Users with password admin access can still do so. Identity storage and offline messages must also be enabled in server settings.

## Powers and protection

The following table is the complete list of **15 server-group power and protection fields**. All values are whole numbers from **0 to 1000**. Simple view offers named numbers; Advanced view lets you enter a number directly.

Talk, channel message, channel picture and streaming power start at 25 for Guest and Normal, 50 for Moderator, and 1000 for Admin. Channel requirements start at 0. Other powers start at 0 unless the selected preset gives a higher value.

| Value | Meaning | Value when omitted from a parsed permission object |
| --- | --- | --- |
| `kick_power` | Power to disconnect another user. Must meet their kick protection. Also requires `kick`. | 0 |
| `kick_protection` | Minimum kick power another user needs to disconnect this user. | 0 |
| `ban_power` | Power to ban another user. Must meet their ban protection. Also requires `ban`. | 0 |
| `ban_protection` | Minimum ban power another user needs to ban this user. | 0 |
| `group_management_power` | Power to change a group or membership. Must meet the protection of the group and affected user. The action also needs its own permission. | 0 |
| `group_protection` | Minimum management power needed to change this group or its users' memberships. | 0 |
| `move_power` | Power to move another user or kick them from a channel using server permissions. Must meet their move protection. | 0 |
| `move_protection` | Minimum power needed for those moves and channel kicks. | 0 |
| `moderation_power` | Power to mute another user or stop their stream using server permissions. Must meet their moderation protection. | 0 |
| `moderation_protection` | Minimum power needed for those mutes and stream stops. | 0 |
| `talk_power` | Must meet the channel requirement to send microphone or separate client music audio. | 25 |
| `channel_message_power` | Must meet the channel requirement to send or edit channel text. | 25 |
| `channel_picture_power` | Must meet the channel requirement to send channel pictures. | 25 |
| `stream_power` | Must meet the channel requirement to share a screen or video file, including its sound. Watching does not need this power. | 25 |
| `permission_edit_power` | Must meet the Grant value of each permission row being changed. The other editing checks also apply. | 0 |

The omitted-field values describe parsing a permission object. They are separate from the explicit values supplied by a new group's preset and from the Grant threshold attached to each row.

Equal values are enough. An action such as kick, ban or group editing also requires its own permission. A user who is not a full server admin cannot kick or ban a full server admin.

The same admin protection applies to moving a user, kicking them from a channel, muting them and stopping their stream. Power 1000 in a custom group does not allow these actions against a full server admin.

## Complete channel action list

These are all **11 action names** accepted in a channel group's `permissions` object. Each uses `grant` or `deny`; an omitted action or input value `default` becomes `deny`. Their names are a subset of the server action list, but their scope is the assigned channel.

| Permission | What it allows in the assigned channel |
| --- | --- |
| `kick_from_channel` | Return another user to root without disconnecting them. Channel kick power and protection apply. |
| `mute_clients` | Temporarily mute another user's microphone and MP3 audio. Channel moderation power and protection apply. |
| `stop_client_streams` | Stop another user's screen or video stream. Channel moderation power and protection apply. |
| `edit_channel_name` | Change this channel's name. |
| `edit_channel_description` | Change this channel's description. |
| `edit_channel_password` | Set, replace or remove this channel's password. |
| `edit_channel_powers` | Change this channel's required talk, message, picture and streaming powers. |
| `edit_channel_options` | Change audio, video, client limits and the selected icon; channel-type changes have additional checks. |
| `delete_channel` | Delete this channel when deletion is allowed. Each child channel needs its own deletion authorization. |
| `assign_channel_groups` | Assign or remove another user's channel group here, within assignment power and protection limits. |
| `bypass_channel_password` | Enter this assigned channel without its password. The saved assignment can apply on entry; capacity limits still apply. |

There are no channel action fields named `create_channel`, `create_temp_channel`, `move_clients`, `kick`, `ban`, `send_channel_pictures` or `send_channel_files`. Those are server actions. Channel text, pictures, voice and streams use the power fields below alongside applicable server permissions and channel requirements.

## Channel power fields

These are all **10 power and protection names** accepted in a channel group's `permissions` object. Each is a whole number from 0 through 1000; omitted fields become 0. The server counterpart identifies the value combined for channel checks, not an additional server-wide right supplied by the channel group.

| Saved field | Meaning in the assigned channel | Server counterpart |
| --- | --- | --- |
| `channel_group_assignment_power` | Power to change another user's channel group. The action also needs its own permission. | `group_management_power` |
| `channel_group_protection` | Minimum power needed to change the user's assignment. Also protects the group definition from server group managers with less power. | `group_protection` |
| `talk_power` | Power for microphone and MP3 audio. | `talk_power` |
| `channel_message_power` | Power for channel text and edits. | `channel_message_power` |
| `channel_picture_power` | Power for channel pictures. | `channel_picture_power` |
| `stream_power` | Power for screen and video sharing. | `stream_power` |
| `channel_kick_power` | Power to return a user to root. Does not allow moving them to another chosen channel. | `move_power` |
| `channel_kick_protection` | Protection from channel kicks. Does not prevent server kicks or moves. | `move_protection` |
| `channel_moderation_power` | Power to mute audio or stop a stream in the channel. | `moderation_power` |
| `channel_moderation_protection` | Protection from those mutes and stream stops. | `moderation_protection` |

Channel actions use **Enable** or **Disable**. A server group's Enable normally allows the action even if the channel group has Disable. Disable with Negate can block it unless the server permission has Skip. Missing channel power values mean 0.

## Saved channel groups

Definitions are saved in `channel_group_definitions`. Each permanent channel has a `channel_groups` array. Each entry contains an identity hash and `role`, the assigned channel group's ID.

Use **Save changes** to submit definition edits. Accepted channel group changes are saved immediately. The server sends definitions with the group list and sends active assignments with login, channel change and assignment updates. It leaves out permission values when the receiver lacks `view_group_permissions`.

## Older group field names

Current settings and messages use `groups`, `group_ids` and `group_id`. Before using a settings file from a version that called groups tags, update those field names. Also use `admin_group_icon_id`, `admin_group_has_icon` and `is_logging_group_changes_enabled` for the corresponding fields. The server does not load their old names.

## Where the code lives

This section is for developers changing the permission system.

The complete lists above match `g_permission_names` and `g_permission_power_names` in [permissions.c](../server-source-code/main/core/permissions.c), and `channel_groups__supports` and `g_channel_group_power_names` in [channel_groups.c](../server-source-code/main/core/channel_groups.c). Those definitions determine the accepted names and the server-to-channel mappings.

| File | What it contains |
| --- | --- |
| `server-source-code/main/definitions.h` | Permission IDs and the structure holding group values. |
| `server-source-code/main/core/permissions.c` | Permission names, defaults, combination rules, power checks and edit checks. |
| `server-source-code/main/core/channel_groups.c` | Channel group definitions, identity assignments and channel permission checks. |
| `server-source-code/main/msg/client/client_msg_moderation.c` | Requests for moves, channel group changes, bans, temporary mutes and music playback. |
| `client-source-code/client-development/browser/main/scripts/app/channel/channel-groups.js` | Channel group editor, icons and client detail labels. |
| `server-source-code/main/core/identity_store.c` | Saved identity memberships. |
| `server-source-code/main/core/server_settings.c` | Loading and saving groups and permissions. |
| `server-source-code/main/msg/client/client_msg_administration.c` | Group and administration requests, permission checks, and accepted changes. |
| `server-source-code/main/msg/server/server_msg_administration.c` | Group lists, current rights, membership updates and access denied messages. |
| `client-source-code/client-development/browser/main/scripts/app/server/groups.js` | Server groups window, permission descriptions and controls. |

The channel and server information request handlers also use the shared permission checks. `base.c` chooses which handler receives a message.

### Locks

Permission checks read shared client and group data. Functions that read clients require the caller to hold the clients lock. `permissions__is_allowed` takes the groups read lock itself.

`permissions__is_allowed_in_groups`, group management checks and permission writers require the caller to hold both locks. Their source comments describe this requirement. Checks of saved memberships then take the identity store mutex, which is another lock.

### Adding a permission

Update its ID, server name, default, request handler check, client description and this guide. Check one allowed request and one refused request. If the permission controls viewing information, also check that the server leaves out data the user cannot view.
