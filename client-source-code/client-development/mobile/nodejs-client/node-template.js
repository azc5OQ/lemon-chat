// this file is the list of files that build-node.py joins into bundle.js, the chat client for Node.js.
// each @@INCLUDE line is replaced by the named file. the files are the browser page's JavaScript without the parts
// that draw the page

// dom-shim.js comes first, because the other files expect the browser objects it creates.
// aes-js.js opens a function around all following files and main.js closes it, so their names stay inside it

/* @@INCLUDE: node/dom-shim.js @@ */

/* @@INCLUDE: scripts/vendor/aes-js.js @@ */
/* @@INCLUDE: scripts/app/utils.js @@ */
/* @@INCLUDE: scripts/app/globals.js @@ */
// sound playback belongs to the visible client
function sounds__play(name)
{}

function sounds__stop(name)
{}
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
