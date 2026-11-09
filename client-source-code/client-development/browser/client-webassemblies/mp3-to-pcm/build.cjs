const path = require('node:path');
const {compile} = require('../compiler.cjs');

compile(__dirname, 'mp3_decoder.wasm', [
    '-target', 'wasm32-freestanding', '-Os', '-fno-builtin', '-nostdlib', '-fvisibility=hidden',
    '-I', path.join(__dirname, 'main/include'), '-I', path.join(__dirname, 'third-party'),
    '-Wl,--no-entry', '-Wl,--export-dynamic', '-Wl,--export=__heap_base',
    path.join(__dirname, 'main/decoder.c')
]);
