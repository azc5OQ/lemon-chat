// Headless bundle template. Mirrors client-development/src/template.html's second <script> block,
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

// its own <script> block in the browser (template.html:87), i.e. outside the factory. main.js:1279
// builds the 8192-bit dh modulus with bigInt at LOAD time, so this has to be here, not later
/* @@INCLUDE: scripts/vendor/biginteger.js @@ */

/* @@INCLUDE: scripts/app/utils.js @@ */
/* @@INCLUDE: scripts/vendor/aes-js.js @@ */
/* @@INCLUDE: scripts/app/encoding.js @@ */
/* @@INCLUDE: scripts/vendor/jsbn.js @@ */
/* @@INCLUDE: scripts/vendor/sha256.js @@ */
/* @@INCLUDE: scripts/vendor/sha1.js @@ */
/* @@INCLUDE: scripts/vendor/md5.js @@ */
/* @@INCLUDE: scripts/vendor/rsa.js @@ */
/* @@INCLUDE: scripts/vendor/rsa-sign.js @@ */
/* @@INCLUDE: scripts/vendor/cryptico.js @@ */
/* @@INCLUDE: scripts/vendor/js-sha256.js @@ */
// in for ONE function: custom_typeof (audio-opus-glue.js:77), which messages.js calls when clamping
// a chat message's font size. it is a general helper that happens to live in the audio file. the
// opus classes around it are only constructed from the worker entry points, which never run here
/* @@INCLUDE: scripts/app/audio-opus-glue.js @@ */

/* @@INCLUDE: scripts/app/messages.js @@ */
/* @@INCLUDE: scripts/app/node-runtime.js @@ */
/* @@INCLUDE: scripts/app/connection-driver.js @@ */
/* @@INCLUDE: scripts/app/android-webview.js @@ */
/* @@INCLUDE: scripts/app/main.js @@ */
