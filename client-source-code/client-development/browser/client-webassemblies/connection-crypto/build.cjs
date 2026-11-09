const path = require("node:path");
const {compile} = require("../compiler.cjs");
const source = path.join(__dirname, "main");
const ciphers = path.join(source, "crypto");
const library = path.join(__dirname, "third-party/mbedtls-3.6.6");
compile(__dirname, "connection_crypto.wasm", [ "-target", "wasm32-wasi", "-mexec-model=reactor", "-O2", "-ffreestanding", "-fno-builtin", "-nostdlib", "-lc", "-DNDEBUG",
    '-DMBEDTLS_CONFIG_FILE="config.h"', "-I", source, "-I", path.join(library, "include"),
    "-I", ciphers, "-I", path.join(__dirname, "third-party/libtomcrypt/src/headers"),
    path.join(ciphers, "ciphers.c"), path.join(ciphers, "keys_session.c"), path.join(source, "runtime.c"),
    ...["cipher_aes.c", "cipher_camellia.c", "cipher_chacha20.c", "cipher_twofish.c", "cipher_serpent.c", "cipher_trivium.c"].map(function(file)
    {
        return path.join(ciphers, file);
    }),
    ...["aes.c", "camellia.c", "chacha20.c", "sha256.c"].map(function(file)
    {
        return path.join(library, "library", file);
    }),
    "-Wl,--no-entry", "-Wl,--export=__heap_base", "-Wl,--export=ciphers__crypt", "-Wl,--export=cipher_trivium__crypt",
    "-Wl,--export=ciphers__sha256", "-Wl,--export=ciphers__hmac", "-Wl,--export=keys_session__mix"]);
