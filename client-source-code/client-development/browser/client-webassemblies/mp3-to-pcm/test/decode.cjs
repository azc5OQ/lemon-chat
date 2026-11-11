const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const file = process.argv[2] || path.join(__dirname, '../build/mp3_decoder.wasm');
const wrapper = fs.readFileSync(path.join(__dirname, '../../../main/scripts/vendor/minimp3.js'), 'utf8');
const Decoder = vm.runInNewContext(wrapper + '\nDecoder;', {Int16Array, Uint8Array});
const mp3 = Buffer.from(fs.readFileSync(path.join(__dirname, 'tone.mp3.base64'), 'utf8'), 'base64');

function decoder(fileName)
{
    return new Decoder(new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(fileName)), {}).exports);
}

const current = decoder(file);
// The first decoding is the reference: every later decoding of the same file must give the same samples.
// The samples are copied, because the decoder may write the next result into the same memory.
const first = current.decode(mp3);
const expected = {numSamples: first.numSamples, pcm: first.pcm.slice()};
assert.ok(expected.pcm.some(sample => Math.abs(sample) > 0.01 * (expected.pcm instanceof Float32Array ? 1 : 32768)), 'Decoded audio must not be silent');
for (let attempt = 0; attempt < 3; attempt++)
{
    const result = current.decode(mp3);
    let differing = 0;
    assert.equal(result.samplingRate, 44100);
    assert.equal(result.numChannels, 2);
    assert.equal(result.numSamples, expected.numSamples);
    assert.ok(result.numSamples > 8000);
    for (let sample = 0; sample < result.pcm.length; sample++)
    {
        differing += result.pcm[sample] === expected.pcm[sample] ? 0 : 1;
    }
    assert.equal(differing, 0, 'Decoding the same file again gave other samples');
}
assert.equal(current.decode(new Uint8Array()).numSamples, 0);
assert.equal(current.decode(new Uint8Array(1024).fill(73)).numSamples, 0);
console.log('PASS: MP3 sample rate, channels, sample count, audible output, equal output for repeated files, empty and malformed input.');
