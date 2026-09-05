// Headless bundle template. Mirrors ../browser/src/template.html's second <script> block,
// minus the rendering and audio halves. Expanded by build-node.py into bundle.js.
//
// ORDER IS LOAD-BEARING, for three separate reasons:
//
//  1. dom-shim.js must run FIRST and OUTSIDE moduleFactory. aes-js.js line 17 does
//     `var global = (self || window || global || {})`, so `window` has to already exist by the time
//     the factory body runs, or `global` comes out as `{}` and every `global.*` read in the tree
//     misses.
//  2. utils.js sits before aes-js.js in the browser too, i.e. OUTSIDE the factory. Keep it there:
//     the workers never get it, and matching that keeps one mental model of what is in scope.
//  3. aes-js.js OPENS `function moduleFactory()` and main.js CLOSES it with `}));`. Everything
//     between is one function body. Nothing may be inserted after main.js that expects to see the
//     factory's locals - there is no scope out there.

/* @@INCLUDE: node/dom-shim.js @@ */

/* @@INCLUDE: scripts/vendor/aes-js.js @@ */
/* @@INCLUDE: scripts/app/utils.js @@ */
/* @@INCLUDE: scripts/app/globals.js @@ */
/* @@INCLUDE: scripts/vendor/sha256.js @@ */
/* @@INCLUDE: scripts/vendor/js-sha256.js @@ */
// the rsa keypair, encrypt, decrypt, signing and the dh modpow all run in rsa_keygen.wasm;
// this one file replaced the whole old jsbn/rsa/cryptico vendor stack (dh itself uses the
// native BigInt, which node has too)
/* @@INCLUDE: scripts/app/rsa-crypto.js @@ */
// in for ONE function: custom_typeof (audio-opus-glue.js:77), which messages.js calls when clamping
// a chat message's font size. it is a general helper that happens to live in the audio file. the
// opus classes around it are only constructed from the worker entry points, which never run here
/* @@INCLUDE: scripts/app/audio-opus-glue.js @@ */

/* @@INCLUDE: scripts/app/messages.js @@ */
// the chat file handlers in messages.js call into this (card markup, header/body crypto), and
// node decrypts direct chat files itself, so it is part of the headless bundle too
/* @@INCLUDE: scripts/app/chat-files.js @@ */
/* @@INCLUDE: scripts/app/android-host.js @@ */
/* @@INCLUDE: scripts/app/connection.js @@ */
/* @@INCLUDE: scripts/app/workers.js @@ */
/* @@INCLUDE: scripts/app/console-log.js @@ */
/* @@INCLUDE: scripts/app/keys.js @@ */
/* @@INCLUDE: scripts/app/channel-tree.js @@ */
/* @@INCLUDE: scripts/app/chat.js @@ */
/* @@INCLUDE: scripts/app/layout.js @@ */
/* @@INCLUDE: scripts/app/voice.js @@ */
/* @@INCLUDE: scripts/app/server-settings-tab.js @@ */
/* @@INCLUDE: scripts/app/dispatch.js @@ */
/* @@INCLUDE: scripts/app/main.js @@ */
