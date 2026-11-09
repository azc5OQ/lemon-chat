const path = require('node:path');
const {compile} = require('../compiler.cjs');
const opus = path.join(__dirname, 'third-party/opus-1.6.1');
const speex = path.join(__dirname, 'third-party/speexdsp-1.2.1');
const exports_ = ['opus_encoder_create', 'opus_encode_float', 'opus_encoder_ctl', 'opus_encoder_destroy',
    'opus_decoder_create', 'opus_decode_float', 'opus_decoder_ctl', 'opus_decoder_destroy',
    'opus_get_version_string', 'speex_resampler_init', 'speex_resampler_destroy',
    'speex_resampler_process_interleaved_float', 'malloc', 'free'];

compile(__dirname, 'libopus.wasm', [
    '-target', 'wasm32-wasi', '-mexec-model=reactor', '-DNDEBUG', '-DOPUS_BUILD', '-DUSE_ALLOCA',
    '-DHAVE_ALLOCA_H', '-DHAVE_LRINTF', '-DHAVE_LRINT', '-DFLOAT_APPROX',
    '-DOUTSIDE_SPEEX', '-DFLOATING_POINT', '-DEXPORT=', '-DRANDOM_PREFIX=speex',
    '-DPACKAGE_VERSION="1.6.1"',
    '-I', path.join(opus, 'include'), '-I', path.join(opus, 'celt'),
    '-I', path.join(opus, 'silk'), '-I', path.join(opus, 'silk/float'),
    '-I', path.join(speex, 'include/speex'), '-I', path.join(speex, 'libspeexdsp'),
    ...require('./sources.json').map(file => path.join(opus, file)),
    path.join(speex, 'libspeexdsp/resample.c'),
    '-Wl,-z,stack-size=1048576', '-Wl,--initial-memory=16777216', '-Wl,--max-memory=268435456',
    ...exports_.map(name => '-Wl,--export=' + name)
]);
