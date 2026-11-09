# Browser MP3 decoder

The browser reads a selected MP3 into memory and decodes it into PCM samples.
`main/decoder.c` provides the buffer and seek/decode API consumed by
`../../main/scripts/vendor/minimp3.js`. The decoder algorithm stays in
`third-party/minimp3/minimp3.h`.

Run `node build.cjs` with Zig 0.15.2, then `node test/decode.cjs`. The isolated
output is `build/mp3_decoder.wasm`. The normal browser build also runs these
checks before publishing and embedding it.

The old archive contained Git histories, an SDL demo player, fonts, a large MP3
corpus, generated wasm and TypeScript/npm build tools. None are needed to compile
this C wrapper. The small synthetic fixture in `test/tone.mp3.base64` is a
0.2-second stereo 440 Hz tone at 44.1 kHz, generated for this test. Tests compare
sample rate, channels, sample count and PCM with the preserved old decoder,
then exercise repeated files and empty/invalid input.

Wrapper source: https://github.com/bashi/minimp3-wasm
Recorded `origin/master` revision: `12cf7fcd80300ce4dc29075d9e3684c2d8989d47`.
Original archive SHA-256:
`0265688ffe68e7956a3848160056c216f96618734a566b71d00c291c59d14d0a`.
The Apache-2.0 wrapper license is retained in LICENSE. Local changes use Allman
formatting, keep the include path pointed at third-party, and trap failed wasm
memory growth before accessing an unallocated buffer. No npm or Binaryen is
needed for this build.
