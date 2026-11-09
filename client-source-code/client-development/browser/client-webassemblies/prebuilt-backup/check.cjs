const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const browser = path.resolve(__dirname, '../..');

function source(file)
{
    return fs.readFileSync(path.join(browser, file), 'utf8');
}

function embedded(file, moduleName, directory)
{
    return source(file).replace('@@WASM:wasm/' + moduleName + '@@',
        fs.readFileSync(path.join(directory, moduleName)).toString('base64'));
}

function checkRSA(directory)
{
    const context = vm.createContext(
    {
        WebAssembly, atob, crypto: crypto.webcrypto,
        sha256:
        {
            hex(value)
            {
                return crypto.createHash('sha256').update(value).digest('hex');
            }
        }
    });
    vm.runInContext(embedded('main/scripts/app/crypto/rsa-crypto.js', 'rsa_keygen.wasm', directory), context);
    const wasm = context.rsa_crypto__get_rsa_keygen_wasm();
    assert.equal(typeof wasm.rsa_keygen__clear_sensitive, 'function', 'RSA fallback must support mandatory secret clearing');
    // Capacities are the seed and RESULT_HEX_MAX arrays in rsa_keygen.c.
    const buffers = [[wasm.rsa_keygen__get_seed_buffer(), 256], [wasm.rsa_keygen__get_modpow_result(), 2100]];
    for (let index = 0; index < 8; index++)
    {
        buffers.push([wasm.rsa_keygen__get_result(index), 2100]);
    }
    for (let index = 0; index < 3; index++)
    {
        buffers.push([wasm.rsa_keygen__get_modpow_buffer(index), 2100]);
    }
    function cleared()
    {
        const memory = new Uint8Array(wasm.memory.buffer);
        for (const [pointer, length] of buffers)
        {
            assert.ok(memory.subarray(pointer, pointer + length).every(value => value === 0), 'RSA temporary buffer retained data');
        }
    }
    for (const bits of [512, 1024])
    {
        const vector = JSON.parse(source('client-webassemblies/rsa-keygen/test/vectors-' + bits + '.json'))[0];
        const key = context.lemon_crypto.generateRSAKey(vector.passphrase, bits);
        for (const field of ['n', 'e', 'd', 'p', 'q', 'dmp1', 'dmq1', 'coeff'])
        {
            assert.equal(key[field].toString(16), vector[field], bits + '-bit identity ' + field);
        }
        cleared();
        for (const message of [2n, 42n, key.n - 1n])
        {
            const ciphertext = key.doPublic(message);
            assert.equal(ciphertext, context.lemon_crypto.modpow(message, BigInt(key.e), key.n));
            cleared();
            assert.equal(key.doPrivate(ciphertext), message);
            cleared();
        }
        assert.equal(key.decrypt(key.encrypt('prebuilt compatibility')), 'prebuilt compatibility');
        cleared();
    }
    assert.throws(() => context.lemon_crypto.generateRSAKey('synthetic rejected request', 1), /rejected/);
    cleared();
    const invalidKey = new context.lemon_crypto.RSAKey();
    invalidKey.setPublic('2', '3');
    assert.throws(() => invalidKey.doPublic(1n), /rejected input/);
    cleared();
    console.log('PASS: prebuilt RSA identity vectors, public/private operations and mandatory cleanup on success/failure.');
}

function checkMP3(directory)
{
    const wasm = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(path.join(directory, 'mp3_decoder.wasm'))), {}).exports;
    const before = wasm.memory.buffer.byteLength;
    for (const size of [64 * 1024 * 1024 + 1, 0xfffffffe, 0xffffffff])
    {
        assert.throws(() => wasm.decoder_set_mp3_data_size(size), WebAssembly.RuntimeError);
        assert.equal(wasm.memory.buffer.byteLength, before, 'oversized input must fail before growing memory');
    }
    const Decoder = vm.runInNewContext(source('main/scripts/vendor/minimp3.js') + '\nDecoder;', {Uint8Array, Int16Array});
    const decoder = new Decoder(wasm);
    const tone = Buffer.from(source('client-webassemblies/mp3-to-pcm/test/tone.mp3.base64'), 'base64');
    for (let attempt = 0; attempt < 3; attempt++)
    {
        const result = decoder.decode(tone);
        assert.equal(result.samplingRate, 44100);
        assert.equal(result.numChannels, 2);
        assert.ok(result.numSamples > 8000);
        assert.ok(result.pcm.some(value => value !== 0));
    }
    assert.equal(decoder.decode(new Uint8Array()).numSamples, 0);
    assert.equal(decoder.decode(new Uint8Array(1024).fill(73)).numSamples, 0);
    console.log('PASS: prebuilt MP3 decoding and oversized/wrapped input limits.');
}

function checkCiphers(directory)
{
    const context = vm.createContext({WebAssembly, atob});
    vm.runInContext(embedded('main/scripts/app/crypto/connection-crypto.js', 'connection_crypto.wasm', directory), context);
    const key = Buffer.from(Array.from({length: 32}, (_, index) => index));
    const iv = Buffer.from(Array.from({length: 16}, (_, index) => 31 - index));
    const data = Buffer.from('A fallback must preserve connection ciphertext.');
    for (const type of ['aes256', 'camellia256', 'chacha20', 'twofish256', 'serpent256', 'trivium80'])
    {
        const encoded = context.connection_crypto__crypt(type, key, iv, data);
        assert.deepEqual(Buffer.from(context.connection_crypto__crypt(type, key, iv, encoded)), data);
        if (['aes256', 'camellia256', 'chacha20'].includes(type))
        {
            const cipherKey = type === 'chacha20'
                ? crypto.createHmac('sha256', key).update('lemonchat-chacha20-iv-v2').update(iv).digest() : key;
            const cipherIV = type === 'chacha20' ? Buffer.alloc(16) : iv;
            const algorithm = type === 'aes256' ? 'aes-256-ctr' : type === 'camellia256' ? 'camellia-256-ctr' : 'chacha20';
            const reference = crypto.createCipheriv(algorithm, cipherKey, cipherIV);
            assert.deepEqual(Buffer.from(encoded), Buffer.concat([reference.update(data), reference.final()]));
        }
        const wasm = context.connection_crypto__module();
        const start = Number(wasm.__heap_base.value);
        assert.ok(new Uint8Array(wasm.memory.buffer, start, data.length + 64).every(value => value === 0));
    }
    console.log('PASS: all six prebuilt connection ciphers, native reference parity and temporary-buffer cleanup.');
}

async function checkOpus(directory)
{
    const quiet =
    {
        log()
        {
        },
        warn()
        {
        },
        error()
        {
        }
    };
    const context = vm.createContext(
    {
        console: quiet, WebAssembly, Uint8Array, Int32Array, Float32Array,
        setTimeout, clearTimeout, setInterval, clearInterval,
        atob, THREAD_NAME: 'prebuilt-check', window: {},
        decode(value)
        {
            return new Uint8Array(Buffer.from(value, 'base64'));
        }
    });
    let timer;
    const ready = new Promise((resolve, reject) =>
    {
        context.Module = {onRuntimeInitialized: resolve, onAbort: reject};
        timer = setTimeout(() => reject(new Error('Prebuilt Opus initialization timed out')), 10000);
    });
    try
    {
        const loader = fs.readFileSync(path.join(directory, 'libopus.emscripten.js'), 'utf8')
            .replace('@@WASM:wasm/libopus.wasm@@', fs.readFileSync(path.join(directory, 'libopus.wasm')).toString('base64'));
        vm.runInContext(loader, context);
        context.create_opus_webassembly_instance();
        await ready;
    }
    finally
    {
        clearTimeout(timer);
    }
    context.global = context;
    vm.runInContext(source('main/scripts/app/audio/audio-opus-glue.js'), context);
    const malloc = context.Module._malloc;
    const free = context.Module._free;
    const allocations = new Set();
    context.Module._malloc = function(size)
    {
        const pointer = malloc(size);
        if (pointer)
        {
            allocations.add(pointer);
        }
        return pointer;
    };
    context.Module._free = function(pointer)
    {
        allocations.delete(pointer);
        return free(pointer);
    };
    for (const channels of [1, 2])
    {
        const encoder = new context.OpusEncoder(2049, 20, 48000, 48000, channels, 0);
        const decoder = new context.OpusDecoder(48000, channels);
        const input = Float32Array.from({length: 960 * channels}, (_, index) => Math.sin(index / channels * 0.05) * 0.2);
        const packets = encoder.encode(input);
        assert.equal(packets.length, 1);
        assert.equal(decoder.decode(packets[0]), 960);
        assert.ok(decoder.pcm.every(Number.isFinite));
        assert.ok(decoder.pcm.some(value => Math.abs(value) > 0.01));
        encoder.destroy();
        decoder.destroy();
        encoder.destroy();
        decoder.destroy();
        assert.equal(allocations.size, 0);
    }
    context.Module._malloc = malloc;
    context.Module._free = free;
    require('../libopusjs/test/resampler-lifecycle.cjs').checkResampler(context, true);
    console.log('PASS: matching prebuilt Opus loader, mono/stereo audio and buffer cleanup.');
}

async function check(directory = __dirname)
{
    const manifest = JSON.parse(fs.readFileSync(path.join(directory, 'sha256.json'), 'utf8'));
    for (const [name, expected] of Object.entries(manifest))
    {
        assert.equal(crypto.createHash('sha256').update(fs.readFileSync(path.join(directory, name))).digest('hex'), expected,
            'Prebuilt checksum mismatch: ' + name);
    }
    checkRSA(directory);
    checkMP3(directory);
    checkCiphers(directory);
    await checkOpus(directory);
}

if (require.main === module)
{
    check().catch(error =>
    {
        console.error(error);
        process.exitCode = 1;
    });
}

module.exports = {check};
