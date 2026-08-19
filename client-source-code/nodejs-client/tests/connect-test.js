// full login against a real server: dh/aes handshake + challenge, no dom, no webworkers
//   node connect-test.js [host] [port] [key,key,...]

let t = require("./test-helpers.js");
t.set_name("CONNECT TEST");

let host = process.argv[2] || "127.0.0.1";
let port = process.argv[3] || "1111";
let metadata_keys = process.argv[4] ? process.argv[4].split(",") : [];

console.log("connecting to ws://" + host + ":" + port + " with " + metadata_keys.length + " metadata key(s)");

let bundle = require("../load-bundle.js")();

bundle.init_node_runtime();
bundle.android_js_bridge.accept_current_settings_from_android(
    t.android_settings({ host: host, port: port, metadata_keys: metadata_keys }));

t.wait_until("logged in", function()
{
    let state = bundle.read_state();
    return state.g_is_channel_list_retrieved === true && state.g_is_client_list_retrieved === true
        && state.g_local_username.length > 0;
}, 30000, function()
{
    let state = bundle.read_state();

    console.log("channels: " + JSON.stringify(state.g_channel_list.map(function(c) { return c.name; })));
    console.log("clients:  " + JSON.stringify(state.g_client_list.map(function(c) { return c.username; })));
    t.check("logged in as \"" + state.g_local_username + "\" with no dom and no webworkers", true);

    // bridge-test needs this client to stay visible past the observer's debounce window
    let linger_ms = parseInt(process.env.LEMONCHAT_LINGER_MS || "0", 10);
    setTimeout(t.finish, (linger_ms > 0) ? linger_ms : 0);
});
