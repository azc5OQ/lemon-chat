// builds the headless shared client bundle

// load the DOM shim before the vendor factory
// the AES vendor file opens the factory and main.js closes it

/* @@INCLUDE: node/dom-shim.js @@ */

/* @@INCLUDE: scripts/vendor/aes-js.js @@ */
/* @@INCLUDE: scripts/app/utils.js @@ */
/* @@INCLUDE: scripts/app/globals.js @@ */
// sound playback belongs to the visible client
function sounds__play(name)
{
}

function sounds__stop(name)
{
}
/* @@INCLUDE: scripts/vendor/sha256.js @@ */
/* @@INCLUDE: scripts/vendor/js-sha256.js @@ */
// use the shared RSA implementation

/* @@INCLUDE: scripts/app/crypto/rsa-crypto.js @@ */
/* @@INCLUDE: scripts/app/crypto/identity-file.js @@ */
// include shared audio helpers without running codec workers

/* @@INCLUDE: scripts/app/audio/audio-opus-glue.js @@ */

/* @@INCLUDE: scripts/app/chat/image-upload.js @@ */
/* @@INCLUDE: scripts/app/server/groups.js @@ */
/* @@INCLUDE: scripts/app/server/moderation.js @@ */
/* @@INCLUDE: scripts/app/channel/channel-groups.js @@ */
/* @@INCLUDE: scripts/app/channel/client-status.js @@ */
/* @@INCLUDE: scripts/app/chat/server-emojis.js @@ */
/* @@INCLUDE: scripts/app/msg/messages.js @@ */
// include attachment handlers and content decryption

/* @@INCLUDE: scripts/app/chat/chat-files.js @@ */
/* @@INCLUDE: scripts/app/mobile/android-host.js @@ */
/* @@INCLUDE: scripts/app/connection/connection.js @@ */
/* @@INCLUDE: scripts/app/workers/local-transfer.js @@ */
/* @@INCLUDE: scripts/app/workers/workers.js @@ */
/* @@INCLUDE: scripts/app/console-log.js @@ */
/* @@INCLUDE: scripts/app/crypto/connection-crypto.js @@ */
/* @@INCLUDE: scripts/app/crypto/keys.js @@ */
/* @@INCLUDE: scripts/app/channel/channel-tree.js @@ */
/* @@INCLUDE: scripts/app/chat/chat.js @@ */
/* @@INCLUDE: scripts/app/chat/chat-reactions.js @@ */
/* @@INCLUDE: scripts/app/chat/chat-replies.js @@ */
/* @@INCLUDE: scripts/app/chat/link-previews.js @@ */
/* @@INCLUDE: scripts/app/ui/layout.js @@ */
/* @@INCLUDE: scripts/app/audio/voice.js @@ */
/* @@INCLUDE: scripts/app/audio/audio-transport.js @@ */
/* @@INCLUDE: scripts/app/video/video-transport.js @@ */
/* @@INCLUDE: node/video-stubs.js @@ */
/* @@INCLUDE: scripts/app/server/server-settings-tab.js @@ */
/* @@INCLUDE: scripts/app/msg/dispatch.js @@ */
/* @@INCLUDE: scripts/app/main.js @@ */
