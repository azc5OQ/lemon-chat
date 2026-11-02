# Server settings

Server settings control which features Lemon Chat offers and the limits that apply to everyone connected. This includes audio and video, uploads, connection methods and user limits.

The server saves these settings in `server_settings.json`. Many can also be changed through the administration window while the server is running.

Local client settings work within the server's rules. A user can choose WebRTC for audio, for example, only when the server allows it.

## Opening and saving settings

For the first login, follow [Administration](administration.md#open-the-admin-settings-with-the-password). It shows the admin-password button, the initial password-change prompt and how to reach the connected administration window.

1. Connect to the server.
2. Click the **lock icon in the upper toolbar** to open **Admin Password**, enter your admin password in the **password** field, and select **send**. The [illustrated login steps](administration.md#open-the-admin-settings-with-the-password) show the icon and dialog.
3. Open the administration window and select **server settings**.
4. Change the options you need, then select **save server settings**.

Once the server accepts the changes, it applies them and sends the relevant rules to connected clients. The controls in this tab work while the server is running.

Saving records the configuration in `server_settings.json`, including permanent channels, icons, groups, custom emojis, bans and identities according to the storage settings.

Temporary channels are not saved. Channel maintainers are chosen again after a restart.

Some options, including ports, connection passwords and the maximum number of clients, are set during setup or in the settings file. They are not all available in the browser tab. To edit the file, stop the server, make the changes, then start it again.

## Usage and public information

The server reports its current usage at the top of the settings tab:

- **Occupied slots:** how many client places are in use.
- **Uptime:** how long the server has been running.
- **RAM usage:** how much memory the server process is using.
- **Received and sent bytes:** data transferred over WebSocket and WebRTC during this server run.

The byte counters measure traffic on the chat connections. Traffic from other programs and the HTTP server is outside those counters.

While the tab is visible, the browser requests new readings about every five seconds. A reading shown as **Unavailable** was not supplied by the server; it is not a measurement of zero. RAM usage is displayed here, but cannot be limited from this tab.

Some server information can also be shared with users. The admin chooses separately whether to show a banner, uptime, occupied and total slots, and the public settings list. The root channel displays the shared information, with **server info** in its menu opening the public settings.

Hiding the public list does not stop clients from receiving the settings they need for their connection and permissions. The admin's RAM and data transfer readings are not included in that public list.

## Available controls

| Section | Settings |
| --- | --- |
| Clients | Country flags, showing the admin flag, country restrictions, renaming, multiple users from one IP address, link previews, and icon or avatar size limits. |
| Chat | Typing indicators, messages to idle users, direct messages, file uploads, pictures shown in chat, and attachment size limits. |
| Voice and channels | Audio, video, music bot audio, connection methods, temporary channels, and hiding members of password protected channels. |
| Attachment receive choices | Whether users may block direct pictures, direct files, channel pictures or channel files on their own device. |
| Connections and security | Simultaneous logins, minimum RSA identity key size, reconnect behavior, replacing an existing connection, and checks that a connection is still active. |
| Channel keys in large channels | Whether to keep channel encryption keys after someone joins or leaves, and the minimum active user counts for doing so. |

For example, if people should attach documents to a conversation, scroll to **chat**, enable **allow file uploads in chat**, and choose the maximum file size. Save the settings after reviewing the values.

![Chat settings showing private messages, file uploads enabled, a 10 MB file limit and a 4 MB picture limit.](images/server-chat-upload-settings.jpg)

*This example allows file uploads up to 10 MB. The file limit and the picture limit are separate controls; the displayed values are example settings.*

### Blocking incoming attachments

Attachment receive settings decide whether users may block incoming pictures or files on their own devices. Enabling one of these options makes the corresponding local choice available to users.

For example, allowing clients to block direct pictures lets one user turn that block on while another leaves it off. The server option allows the choice; it does not block pictures for everyone.

These choices apply to new transfers. They do not remove pictures or files already received.

### Link previews

Link previews are looked up by the server. When the feature is enabled, the client sends the posted URL separately for that lookup. The server can therefore read the URL even though the chat message containing it is encrypted.

## Audio and video connections

Audio and video can use WebSocket or WebRTC DataChannel. The server's connection selector decides which of these methods clients may use:

- **WebSocket only**
- **WebRTC only**
- **WebSocket and WebRTC (clients choose)**

This choice applies to audio and video. The main chat connection always uses WebSocket.

![Voice and channels settings with audio and video enabled and audio and video connections set to WebSocket only.](images/server-audio-video-settings.jpg)

*Find **audio and video connections** under **voice and channels**. Here the server allows both audio and video, using WebSocket only. Save the settings to apply your selection.*

Choosing **WebSocket only** allows audio and video without WebRTC. The client opens separate audio and video WebSockets at the same server address and port as the main chat connection.

With both methods allowed, the browser tries the user's preferred method first. If it has not connected after about ten seconds, it also tries the other method.

The server forwards packets between clients using either method. Users on WebRTC and WebSocket can therefore hear each other and watch the same video. Each connection ends at the server, so users do not exchange their IP addresses through WebRTC.

The [WebRTC guide](webrtc.md) explains the DataChannel design, the built-in Violet STUN service, and why clients do not exchange IP addresses. For an HTTP tunnel such as Tailscale Funnel, use **WebSocket only**. See the [selfhosting guide](../selfhosting-tools/README.md) for ports and commands.

The server and the channel must both allow audio before users can send audio. The same rule applies to video.

Video streaming is off in the server's default settings. The browser's **Create Channel** dialog also starts with **Enable Streaming** off, so enable streaming on both the server and the channel. A channel can have only one screen share or video stream at a time.

## Connection handling

### Login and identity options

**Maximum simultaneous logins** limits the number of login calculations running at once. It accepts 1 to 64 and defaults to 4. The total client limit is separate, so a server with room for 100 connected clients can process four new logins at a time.

**Minimum RSA key size** sets the minimum accepted size of a client's identity key. A separate option decides whether the server tells a rejected client which size is required.

**Fast reconnect** lets a returning client resume a session that the server still has open. The client must use the same identity and prove again that it holds the private key.

A successful reconnect keeps the client ID, channel, name, groups and roles, but establishes new connection keys. Any upload on the old connection is stopped and must be sent again. A session that has already ended cannot be resumed. Fast reconnect is off by default.

**Identity takeover** lets a new connection replace an existing connection using the same identity. It also requires proof of the private key. The old session closes and the replacement begins as a new login in root. This option is off by default.

Fast reconnect therefore continues a session, while takeover replaces it. If neither is allowed or succeeds, the identity must wait for its earlier session to end or time out before connecting again.

Allow multiple clients from the same IP address when several people share an internet connection or when testing several clients on the server computer.

### Checking whether a connection is still active

Clients send regular keep-alive messages so the server can check that the connection still works. **Disconnect clients when keep-alive messages time out** closes a client's connection when those messages stop arriving in time. It is on by default and can be changed while the server runs.

**Additional WebSocket ping check** is a separate connection check. It is off by default. Changing one of these options does not change the other.

### Waiting after failed WebRTC connections

The server counts failed WebRTC connection attempts for each client. After ten failures, **Voice datachannel cooldown after 10 failed connection attempts** determines how long that client must wait before trying again. This limits the repeated setup work when a connection cannot be established.

The default wait is 600 seconds, or ten minutes. Setting it to 0 removes the waiting period. A successful connection resets the failure count.

### Tailscale Funnel

Tailscale Funnel mode uses the local Tailscale installation to forward public HTTPS and WSS connections to Lemon Chat. It is enabled by choosing Funnel during setup or setting `is_tailscale_funnel_enabled` in the settings file. The default is off, and changing the file requires a restart.

Tailscale must already be installed, running and signed in. At startup, Lemon Chat asks it to forward these public connections:

| Public connection | Local destination |
| --- | --- |
| HTTPS on port 443 | `http_port` |
| WSS on `wss_port` | `websocket_port` |

The startup summary prints the public addresses after checking both routes. If the check fails, Lemon Chat ignores forwarded IP headers for that run.

With Funnel enabled and verified, a WebSocket connection from `127.0.0.1` can provide one client IP address in the `X-Forwarded-For` header. This header lets the local proxy report the original user's address. Connections from other addresses cannot use it. Local connections without the header are still accepted, and plain WS access remains available.

Use Funnel's HTTP proxy mode. Programs on the server computer must be trusted because local connections can provide this header. See the [Funnel instructions](../selfhosting-tools/README.md#client-ip-addresses).

## Channel keys in large channels

The server chooses one available client in each channel at random to be its **maintainer**. That client creates the channel encryption keys, keeps a copy and sends a separate encrypted copy to every other member who is not idle or a music bot.

Normally this work is repeated when someone joins or leaves. In large channels it can delay messages, so the server can allow existing keys to be kept after a membership change. A new member still receives their own encrypted copy.

The [channel guide](channels.md#the-channel-maintainer) explains who can be chosen and how clients request a replacement when keys do not arrive.

| Setting | Default |
| --- | --- |
| Enable channel key reuse | On |
| Keep keys when someone joins | On, with at least 50 active users after the join |
| Keep keys when someone leaves, disconnects or enters idle mode | On, with at least 100 active users after the change |
| Smallest allowed minimum | 30 active users |

Idle users and music bots are not counted. Below the relevant minimum, new keys are required. A new maintainer always creates new keys. Saved changes to these rules are sent to connected clients immediately.

For example, 101 active users are in a channel. A user who is not the maintainer leaves and 100 remain. With the default settings, the remaining users can keep the same keys. If another user leaves and 99 remain, new keys are required. If the maintainer leaves, new keys are required regardless of the number of users.

Keeping keys has these effects:

- A new member who obtains earlier data encrypted with those keys may be able to read it.
- A former member who obtains later data encrypted with those keys may be able to read it until the keys change.

Leaving a channel stops the server from sending that channel's messages to the user. See [Encryption](encryption.md) for the different keys and what they protect.

## Other administration tabs

**Server groups** opens the window for permissions, memberships and server icons. Its member list includes saved identities as well as connected users.

**Bans** manages blocked users or addresses. **Emojis** manages the reaction images available on this server.

**Log** controls which administration events are recorded and how much history is kept. Log entries stay in RAM. Logging settings are saved, but the entries themselves are not written to the settings file.

![Server log tab showing a 10 MB limit, seven-day retention, save log settings and refresh controls, and example connection and settings events.](images/server-log.jpg)

*Scroll down in **log** to find the size and retention controls and recent entries. Use **save log settings** for these options and **refresh** to fetch current events. These entries come from the demonstration server.*

Defaults in this guide apply when no saved value replaces them. An existing server uses its saved configuration.

## Optional groups and permissions

Server groups, group icons and group permissions can be enabled or disabled together. The first setup question saves this choice as `is_permission_system_enabled` in `server_settings.json`. Older files without the field keep groups enabled.

With the value `false`, the client hides the group window, group symbols and icon controls. Group permissions and powers do not apply. Server settings still apply. Managing the server requires the admin password; saved Admin group membership does not give admin access in this mode.

Groups, icons and assignments remain saved. To use them again, stop the server, change the value to `true`, and restart it. This setting is not available in the browser and cannot change while the server runs.

## Group permissions

The server settings window opens the group editor, but groups use their own controls and save buttons. Server groups apply across the server. Channel groups apply only in the channel where they are assigned.

Use the [Permissions guide](permissions.md) when you want to create a group, assign members or understand Enable, Disable, power, Negate, Skip and Grant. Use the [permission reference](permission-reference.md) for every accepted field name, saved value and compatibility note.

Under **Appearance** in local settings, **hide server icons in the channel list** hides server group and custom channel icons on that device. It does not change group membership or permissions.

## Server reaction emojis

The server supplies the emojis that users can attach to messages as reactions. These can be text symbols or uploaded images, and appear in the client's emoji selection window.

Full server admins manage this list through **Emojis** in server settings. Groups can also give other users **Upload emojis** or **Delete emojis** separately. Those users reach the available controls through **Server moderation** in local settings.

The emoji feature also works when groups are disabled. In that mode, managing emojis requires password admin access.

![Emojis administration tab with name and image fields, an Upload button, search, and the server emoji list.](images/server-emojis.jpg)

*Open **emojis** to name and upload a reaction image or search the existing list. After changing the list, scroll down to **Save changes** to keep it after a restart.*

### Default emojis

Without a saved list, the server starts with 30 standard Unicode emojis displayed as text symbols by the client. They can be deleted from the list in the same way as uploaded images.

Once a list is saved, the server uses it instead of the defaults. Saving an empty list therefore keeps the list empty after a restart.

### Upload limits

For a still image, select a PNG, JPEG or WebP file up to 10 MiB. The client prepares it for upload with a maximum size of 128 by 128 pixels and 30,000 bytes. Animated WebP is not supported.

For animation, select a GIF. It keeps its animation and must already fit these limits: 128 by 128 pixels, 256 KiB and 200 frames. A frame is one image in the animation.

MiB and KiB are file size units: 1 MiB is 1,048,576 bytes and 1 KiB is 1,024 bytes.

Emoji names, such as `coffee`, must contain 1 to 32 lowercase letters, numbers or underscores. The server supports up to 256 emojis, with search and pages in the admin list.

### Saving and deleting emojis

Uploads and deletions affect connected clients immediately. Select **Save changes** to keep them after a restart. The server stores the list in the `emojis` array in `server_settings.json`, separately from group icons.

Clients receive names, permanent IDs and any Unicode symbols. They download image data when needed and keep a local copy for the current server.

Deleting an emoji removes it from the selection window. Earlier reactions keep their counts and show the locally stored image or saved name. Uploading a new image does not replace those earlier reactions.

### Using reactions

Enable **emoji reactions** in local settings. Select a chat message to open the available reply and reaction controls.

Reaction details are sent inside the encrypted channel or private message. The server supplies the emoji list and images, but does not read the encrypted reaction contents.

## Relevant source files

- [Loading, defaults and saving](../server-source-code/main/core/server_settings.c)
- [Admin settings requests](../server-source-code/main/msg/client/client_msg_server_settings.c)
- [Settings and public information sent to clients](../server-source-code/main/msg/server/server_msg_server_settings.c)
- [Settings tab behavior](../client-source-code/client-development/browser/src/scripts/app/server-settings-tab.js)
- [Usage readings and public server information](../client-source-code/client-development/browser/src/scripts/app/server-info.js)
