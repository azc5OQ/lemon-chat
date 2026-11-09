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
const previous = decoder(path.join(__dirname, '../../prebuilt-backup/mp3_decoder.wasm'));
const expected = previous.decode(mp3);
for (let attempt = 0; attempt < 3; attempt++)
{
    const result = current.decode(mp3);
    assert.equal(result.samplingRate, 44100);
    assert.equal(result.numChannels, 2);
    assert.equal(result.numSamples, expected.numSamples);
    assert.ok(result.numSamples > 8000);
    let squaredError = 0;
    for (let sample = 0; sample < result.pcm.length; sample++)
    {
        squaredError += (result.pcm[sample] - expected.pcm[sample]) ** 2;
    }
    assert.ok(Math.sqrt(squaredError / result.pcm.length) < 2, 'Decoded PCM changed unexpectedly');
}
assert.equal(current.decode(new Uint8Array()).numSamples, 0);
assert.equal(current.decode(new Uint8Array(1024).fill(73)).numSamples, 0);
console.log('PASS: MP3 sample rate, channels, sample count, PCM parity, repeated files, empty and malformed input.');
