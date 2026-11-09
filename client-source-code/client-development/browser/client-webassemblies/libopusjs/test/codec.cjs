const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const {checkResampler} = require('./resampler-lifecycle.cjs');
const browser = path.resolve(__dirname, '../../..');
const file = process.argv[2] || path.join(__dirname, '../build/libopus.wasm');
const context = vm.createContext({console, WebAssembly, Uint8Array, Int32Array, Float32Array,
    atob});
const runtime = fs.readFileSync(path.join(browser, 'main/scripts/app/audio/opus-runtime.js'), 'utf8')
    .replace('@@WASM:wasm/libopus.wasm@@', fs.readFileSync(file).toString('base64'));
vm.runInContext(runtime, context);
const current = context.create_opus_webassembly_instance();
const versionMemory = new Uint8Array(current.memory.buffer);
const versionStart = current.opus_get_version_string();
assert.match(Buffer.from(versionMemory.subarray(versionStart, versionMemory.indexOf(0, versionStart))).toString(), /1\.6\.1/);

// Cross-decode against the previously shipped 1.5.2 module in both directions.
let previous;
const imports =
{
    env:
    {
        emscripten_memcpy_big(destination, source, count)
        {
            new Uint8Array(previous.memory.buffer).copyWithin(destination, source, source + count);
            return destination;
        },
        emscripten_resize_heap(size)
        {
            previous.memory.grow(Math.ceil((size - previous.memory.buffer.byteLength) / 65536));
            return 1;
        },
        abort()
        {
            throw new Error('Previous Opus module aborted');
        }
    },
    wasi_snapshot_preview1:
    {
        fd_close: () => 0,
        fd_write: () => 0,
        fd_seek: () => 0
    }
};
previous = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(
    path.join(__dirname, '../../prebuilt-backup/libopus.wasm'))), imports).exports;
previous.__wasm_call_ctors();

for (const channels of [1, 2])
{
    for (const frames of [120, 240, 480, 960, 1920, 2880])
    {
        for (const [encode, decode] of [[current, previous], [previous, current], [current, current]])
        {
            const error = encode.malloc(4);
            const encoder = encode.opus_encoder_create(48000, channels, 2049, error);
            assert.ok(encoder);
            const pcm = encode.malloc(frames * channels * 4);
            const packet = encode.malloc(4000);
            const values = new Float32Array(encode.memory.buffer, pcm, frames * channels);
            for (let index = 0; index < values.length; index++)
            {
                values[index] = Math.sin(Math.floor(index / channels) * 440 * 2 * Math.PI / 48000) * 0.25;
            }
            new Int32Array(encode.memory.buffer)[error >> 2] = 64000;
            assert.equal(encode.opus_encoder_ctl(encoder, 4002, error), 0);
            const bytes = encode.opus_encode_float(encoder, pcm, frames, packet, 4000);
            assert.ok(bytes > 0 && bytes <= 4000);
            const encoded = new Uint8Array(encode.memory.buffer, packet, bytes).slice();
            const decodeError = decode.malloc(4);
            const decoder = decode.opus_decoder_create(48000, channels, decodeError);
            assert.ok(decoder);
            const input = decode.malloc(bytes);
            const output = decode.malloc(frames * channels * 4);
            new Uint8Array(decode.memory.buffer, input, bytes).set(encoded);
            assert.equal(decode.opus_decode_float(decoder, input, bytes, output, frames, 0), frames);
            const samples = new Float32Array(decode.memory.buffer, output, frames * channels);
            assert.ok(samples.every(Number.isFinite));
            if (frames >= 960)
            {
                assert.ok(samples.some(value => Math.abs(value) > 0.01), 'Decoded audio must not be silent');
            }
            for (let lost = 0; lost < 2; lost++)
            {
                assert.equal(decode.opus_decode_float(decoder, 0, 0, output, frames, 0), frames);
                assert.ok(samples.every(Number.isFinite));
            }
            assert.equal(decode.opus_decoder_ctl(decoder, 4028, 0), 0);
            assert.equal(encode.opus_encoder_ctl(encoder, 4028, 0), 0);
            encode.opus_encoder_destroy(encoder);
            decode.opus_decoder_destroy(decoder);
            for (const pointer of [error, pcm, packet])
            {
                encode.free(pointer);
            }
            for (const pointer of [decodeError, input, output])
            {
                decode.free(pointer);
            }
        }
    }
}

// Exercise the actual browser resampler and heap-view updates, including growth.
vm.runInContext(`
    create_opus_webassembly_instance();
    for (const channels of [1, 2])
    {
        const resampler = new SpeexResampler(channels, 44100, 48000);
        const input = new Float32Array(4410 * channels).fill(0.2);
        const first = resampler.process(input).slice();
        if (first.length !== 4800 * channels || !first.every(Number.isFinite))
        {
            throw new Error('Incorrect resampler output');
        }
        const oldBuffer = Module.HEAPF32.buffer;
        const large = Module._malloc(Module.HEAPU8.byteLength + 1024 * 1024);
        if (oldBuffer === Module.HEAPF32.buffer)
        {
            throw new Error('Test did not exercise memory growth');
        }
        Module._free(large);
        const second = resampler.process(input);
        if (second.length !== first.length || !second.every(Number.isFinite))
        {
            throw new Error('Resampler failed after memory growth');
        }
        resampler.destroy();
        resampler.destroy();
    }
`, context);
console.log('PASS: Opus 1.6.1 mono/stereo, all frame sizes, old/new interoperability, bitrate/reset, PLC, resampling and heap growth.');
checkResampler(context);
