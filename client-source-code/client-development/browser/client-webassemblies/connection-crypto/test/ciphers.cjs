const fs = require('node:fs');
const vm = require('node:vm');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const path = require('node:path');
const prefix = path.resolve(__dirname, '../../../main') + '/';
const wasmFile = process.argv[2] || path.join(__dirname, '../build/connection_crypto.wasm');
const source = fs.readFileSync(prefix + 'scripts/app/crypto/connection-crypto.js', 'utf8').replace('@@WASM:wasm/connection_crypto.wasm@@', fs.readFileSync(wasmFile).toString('base64'));
vm.runInThisContext(source);
const hex = value => Buffer.from(value).toString('hex');
function verify(type, key, iv, expected)
{
    assert.equal(hex(connection_crypto__crypt(type, Buffer.from(key, 'hex'), Buffer.from(iv, 'hex'), Buffer.alloc(16))), expected, type);
}
verify('serpent256', '80' + '00'.repeat(31), '00'.repeat(16), 'a223aa1288463c0e2be38ebd825616c0');
verify('twofish256', 'd43bb7556ea32e46f2a282b7d45b4e0d57ff739d4dc92c1bd7fc01700cc8216f', '90afe91bb288544f2c32dc239b2635e6', '6cb4561c40bf0a9705931cb6d408e7fa');
for (const type of ['aes256', 'camellia256', 'chacha20'])
{
    for (const size of [0, 1, 15, 16, 17, 63, 64, 65, 1024, 65537])
    {
        const key = crypto.randomBytes(32);
        const iv = crypto.randomBytes(16);
        const data = crypto.randomBytes(size);
        let referenceKey = key;
        let referenceIV = iv;
        if (type === 'chacha20')
        {
            referenceKey = crypto.createHmac('sha256', key).update('lemonchat-chacha20-iv-v2').update(iv).digest();
            referenceIV = Buffer.alloc(16);
        }
        const cipher = crypto.createCipheriv(type === 'aes256' ? 'aes-256-ctr' : type === 'camellia256' ? 'camellia-256-ctr' : 'chacha20', referenceKey, referenceIV);
        const expected = Buffer.concat([cipher.update(data), cipher.final()]);
        const actual = connection_crypto__crypt(type, key, iv, data);
        assert.equal(hex(actual), hex(expected), type + ':' + size);
        assert.equal(hex(connection_crypto__crypt(type, key, iv, actual)), hex(data));
    }
}
// Original decryptor fixture: its ten-byte test key is hashed inside the routine.
const moduleBytes = fs.readFileSync(wasmFile);
const instance = new WebAssembly.Instance(new WebAssembly.Module(moduleBytes), {});
const start = Number(instance.exports.__heap_base.value);
const memory = new Uint8Array(instance.exports.memory.buffer);
memory.set(Buffer.from('17696850441596006280', 'hex'), start);
memory.fill(0, start + 32, start + 132);
assert.equal(instance.exports.cipher_trivium__crypt(start, 10, start + 32, 100), 1);
const expectedTrivium = [99,255,76,234,79,204,156,128,140,153,158,15,109,250,221,179,240,56,20,172,52,16,73,180,87,216,212,180,47,160,102,188,193,142,102,16,170,175,216,199,85,155,238,207,32,118,197,161,253,203,10,160,145,210,219,202,171,228,239,245,192,65,95,89,190,73,201,245,31,47,108,194,140,61,137,191,135,92,23,208,184,234,32,119,59,17,107,29,162,49,248,157,219,126,238,11,152,174,120,236];
assert.deepEqual(Array.from(memory.slice(start + 32, start + 132)), expectedTrivium);
const triviumKey = crypto.randomBytes(32);
const triviumData = crypto.randomBytes(10003);
const triviumEncrypted = connection_crypto__crypt('trivium80', triviumKey, Buffer.alloc(16), triviumData);
assert.equal(hex(connection_crypto__crypt('trivium80', triviumKey, Buffer.alloc(16, 255), triviumData)), hex(triviumEncrypted), 'Exact compatibility deliberately ignores the envelope IV');
assert.equal(hex(connection_crypto__crypt('trivium80', triviumKey, Buffer.alloc(16), triviumEncrypted)), hex(triviumData));
console.log('PASS: Twofish/Serpent known vectors; AES/Camellia/ChaCha20 OpenSSL parity; exact 100-byte Trivium-80 fixture, fixed IV and round trips.');
