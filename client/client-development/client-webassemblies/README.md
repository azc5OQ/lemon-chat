# Client WebAssembly files

`client.html` uses two WebAssembly modules that are embedded directly as
Base64 strings. This way the client does not need to load any external
resources — everything lives inside the single `client.html` file.

The two modules are **minimp3** and **libopus**.

## minimp3 — MP3 streaming

Lets the client pick an MP3 file from disk (similar to selecting an image) and
stream it to the other clients in the channel.

This feature predates the music-bot feature. Now that music bots exist, it is
arguably redundant. The original idea was to let users easily grab an MP3 from
their system and send it to everyone in the channel — much like the old
TeamSpeak days when people streamed music through their microphone.

## libopus (Opus) — microphone & voice audio

Enables the client to use the microphone and to receive audio from other
clients. Received audio is decrypted with the channel keys into Opus, then
decoded into PCM and played back.

## FAQ

### What is WebAssembly?

A file with a `.wasm` extension that modern browsers can load for tasks that
need near-native performance. It is commonly used for things like web-based
emulators (e.g. Flash emulators). Audio processing is hard to do efficiently in
pure JavaScript, so WebAssembly is used instead.

### Is it possible to remove them from `client.html`?

Yes — it is quite easy.

**To remove libopus:**

1. Open `client.html` in VS Code or a similar editor.
2. Locate the function `instantiate_libopus_webassembly_from_base64`.
3. Remove or comment out `libopus_webassembly_base64`.

This breaks audio sending and receiving, but text chat still works.

**To remove minimp3** (likely unnecessary anyway):

1. Locate the function `mp3_to_pcm_worker_onmessage`.
2. As with Opus, remove or comment out the WebAssembly variable.

### How large are they?

| Module  | Size in `client.html` (Base64-encoded) |
| ------- | -------------------------------------- |
| libopus | ~380 KB                                |
| minimp3 | ~30 KB                                 |

### How are these files built?

The build must be done on **Linux**, but the resulting files can be used
anywhere.

1. Download [Emscripten (emsdk)](https://emscripten.org/) and follow its setup
   instructions.
2. Extract the WebAssembly source archives shipped here and build them:
   - `libopusjs/libopus-workingbuild.tar.gz`
   - `mp3-to-pcm/minimp3-wasm.tar.gz`
3. The build produces a `.wasm` file.
4. Convert that `.wasm` into a Base64 string with the included
   [`wasmstob64.py`](wasmstob64.py) script, then paste it into `client.html`.

The archives of both projects the WebAssembly files were built from are
included in this directory.
