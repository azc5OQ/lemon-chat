// Loads bundle.js the way a browser page does (vm, NOT require) and hands back the export seam.
// Under require() the vendor umds see `module` and export themselves instead of assigning the
// bare-name globals (aesjs, _sha256, bigInt) the app uses - a ReferenceError deep inside the
// first encrypt call. A page has no `module`; vm.runInThisContext reproduces that.

const fs = require("fs");
const path = require("path");
const vm = require("vm");

// node 18 (nodejs-mobile) lacks BOTH browser globals the client leans on: WebSocket (21/22+) and
// webcrypto's `crypto` (19+). desktop node 24 has them, which is why only the device ever noticed
function install_crypto_polyfill()
{
    if (typeof globalThis.crypto === "undefined")
    {
        globalThis.crypto = require("crypto").webcrypto;
        return "webcrypto polyfill";
    }

    return "native";
}

function install_websocket_polyfill()
{
    // set LEMONCHAT_FORCE_WS_POLYFILL=1 to take the android path on a desktop node that has a native
    // WebSocket. without it the android runtime is the only place this code ever runs, i.e. the one
    // place it cannot be tested before shipping
    let force_polyfill = (process.env.LEMONCHAT_FORCE_WS_POLYFILL === "1");

    if (typeof globalThis.WebSocket !== "undefined" && force_polyfill === false)
    {
        return "native";
    }

    // zero-dependency client, so the project rebuilds offline without npm
    globalThis.WebSocket = require("./mini-ws.js").WebSocketClient;
    return "mini-ws";
}

function load_bundle()
{
    install_crypto_polyfill();
    install_websocket_polyfill();

    const bundle_path = path.join(__dirname, "bundle.js");
    const source = fs.readFileSync(bundle_path, "utf8");

    vm.runInThisContext(source, { filename: bundle_path });

    if (globalThis.what == null)
    {
        throw new Error("bundle loaded but globalThis.what is unset - the export seam at the end of "
            + "main.js did not run, or the umd in aes-js.js took a different branch");
    }

    return globalThis.what;
}

module.exports = load_bundle;
