# Browser Opus codec

The browser needs to encode microphone samples, decode voice/music packets and
resample microphone rates such as 44.1 kHz to the 48 kHz wire format.
`build.cjs` compiles the local sources with Zig 0.15.2 and exports only the codec,
resampler, allocation and version functions used by the browser.

`third-party/opus-1.6.1` contains the ordinary floating-point mono/stereo codec.
`sources.json` lists its compiled C files. Neural enhancements, DRED models,
96 kHz extensions, multistream/projection APIs and architecture-specific assembly
are not enabled. The Speex dependency contains only its standalone resampler;
we do not need the Speex speech codec or its echo canceller.

Run `node build.cjs`, then `node test/codec.cjs` for an isolated build and test.
The result is `build/libopus.wasm`; the normal browser build tests and publishes
it before embedding. `../../main/scripts/app/audio/opus-runtime.js` is the small
browser loader. It needs no generated Emscripten JavaScript or host imports.

The test encodes and decodes in both directions with the preserved 1.5.2 module,
checks all six frame sizes in mono/stereo, changes bitrate, resets state,
conceals missing packets and resamples while forcing wasm heap growth.
The synthesized audio test is a functional check, not a listening-quality score.
