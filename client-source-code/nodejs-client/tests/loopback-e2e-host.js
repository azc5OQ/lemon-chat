// host for the browser e2e: node logs into the real server, serves the loopback ui, stays alive
//   node loopback-e2e-host.js    (server on 1111; prints E2E_READY <port> <token>)

let t = require("./test-helpers.js");

let bundle = require("../load-bundle.js")();

bundle.init_node_runtime();

// server first, then connect - the android order, so a ui can attach at any point
require("../loopback-ui-server.js").start(bundle, function(port, token)
{
    console.log("E2E_READY " + port + " " + token);
    bundle.android_js_bridge.accept_current_settings_from_android(t.android_settings());
});

setInterval(function() {}, 60000);
