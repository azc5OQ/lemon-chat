// this file runs bundle.js, the chat client, in Node.js the way a browser runs a page script.
// in a browser all scripts share one set of global names, while Node.js gives each file its own. so this file runs
// bundle.js with one shared set of global names, and the libraries in it create their global objects as in a browser

const fs = require("fs");
const path = require("path");
const vm = require("vm");

/**
 * @brief provides Web Crypto when the runtime does not define it
 *
 * @return the selected crypto implementation name
 */
function install_crypto_polyfill()
{
    if (typeof globalThis.crypto === "undefined")
    {
        globalThis.crypto = require("crypto").webcrypto;
        return "webcrypto polyfill";
    }

    return "native";
}

/**
 * @brief installs the packaged WebSocket adapter
 *
 * @return the selected socket implementation name
 */
function install_websocket_polyfill()
{
    // use the packaged socket implementation in Android and desktop tests

    globalThis.WebSocket = require("./websocket-adapter.js").WebSocketClient;
    return "ws 8.23.0 (pure JavaScript)";
}

/**
 * @brief loads the shared client and reads its exports
 *
 * @return shared client exports. throws when loading or initialization fails
 */
function load_bundle()
{
    install_crypto_polyfill();
    install_websocket_polyfill();

    const bundle_path = path.join(__dirname, "bundle.js");
    const source = fs.readFileSync(bundle_path, "utf8");

    vm.runInThisContext(source, { filename: bundle_path });

    if (globalThis.what == null)
    {
        throw new Error("bundle loaded but globalThis.what is unset - the export seam at the end of " + "main.js did not run, or the umd in aes-js.js took a different branch");
    }

    return globalThis.what;
}

module.exports = load_bundle;
