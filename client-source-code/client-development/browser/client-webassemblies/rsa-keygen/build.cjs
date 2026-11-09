const path = require('node:path');
const {compile} = require('../compiler.cjs');

// The seeded prime walk needs exact IEEE doubles to preserve existing identities.
compile(__dirname, 'rsa_keygen.wasm', [
    '-target', 'wasm32-freestanding', '-ffp-contract=off', '-fno-builtin', '-nostdlib',
    '-Wl,--no-entry', '-Wl,--export-dynamic', path.join(__dirname, 'main/rsa_keygen.c')
]);
