// the race the phone hit: a ui attaches BEFORE node logs in. the deferred burst used to fire on
// the auth frame, when node's state was still empty - the ui rendered zero channels forever.
//   node deferred-burst-test.js      (a server must be listening on 1111)

let t = require("./test-helpers.js");
t.set_name("DEFERRED BURST TEST");

let WebSocket = require("../mini-ws.js").WebSocketClient;

let bundle = require("../load-bundle.js")();

bundle.init_node_runtime();

// loopback up FIRST, ui attached BEFORE any settings - exactly the app's startup order
require("../loopback-ui-server.js").start(bundle, function(port, token)
{
    let frames = [];
    let ui = new WebSocket("ws://127.0.0.1:" + port);

    ui.on("open", function() { ui.send(token); });
    ui.on("message", function(data)
    {
        try { frames.push(JSON.parse(data.toString("utf8"))); } catch (e) { }
    });

    // only now hand node the server details, so login happens with the ui already attached
    setTimeout(function()
    {
        bundle.android_js_bridge.accept_current_settings_from_android(t.android_settings());
    }, 1500);

    t.wait_until("burst arrived", function()
    {
        return frames.some(function(f) { return f.message && f.message.type === "client_list"; });
    }, 40000, function()
    {
        // loopback_status frames are meta traffic and may arrive any time - the burst-order
        // guarantee applies to the protocol frames only
        let protocol_frames = frames.filter(function(f)
        {
            return f.message && f.message.type !== "loopback_status";
        });

        let auth = protocol_frames.findIndex(function(f) { return f.message.type === "authentication_status"; });
        let channel_list = protocol_frames.find(function(f) { return f.message.type === "channel_list"; });
        let client_list = protocol_frames.find(function(f) { return f.message.type === "client_list"; });

        t.check("auth frame leads the burst", auth === 0, "auth at index " + auth);

        t.check("node reported its connection status to the ui",
            frames.some(function(f) { return f.message && f.message.type === "loopback_status"; }));

        t.check("channel_list carries the root channel",
            channel_list != null && channel_list.message.channels.length >= 1
            && channel_list.message.channels.some(function(c) { return c.is_root_channel === true; }),
            "channels: " + (channel_list ? channel_list.message.channels.length : "none"));

        t.check("client_list carries at least ourselves",
            client_list != null && client_list.message.clients.length >= 1
            && typeof client_list.message.local_username === "string"
            && client_list.message.local_username.length > 0,
            "clients: " + (client_list ? client_list.message.clients.length : "none"));

        // private messages are rsa-encrypted TO the recipient in the ui, so every replayed
        // client must carry its public key - without it the pm send dies silently
        t.check("replayed clients carry public keys",
            client_list != null && client_list.message.clients.every(function(c)
            {
                return typeof c.public_key === "string" && c.public_key.length > 50;
            }),
            client_list ? JSON.stringify(client_list.message.clients.map(function(c)
            {
                return { id: c.client_id, key: (typeof c.public_key === "string") ? c.public_key.length + " chars" : String(c.public_key) };
            })) : "no client_list");

        // the maintainer frame arrives right after client_list and used to fall into the gap
        // between "burst state built" and "live forwarding on" - keys got rejected without it
        setTimeout(function()
        {
            let maintainer = frames.find(function(f)
            {
                return f.message && f.message.type === "channel_maintainer_id" && f.message.has_maintainer === true;
            });

            t.check("the ui learned who the maintainer is", maintainer != null,
                "no channel_maintainer_id among " + frames.length + " frame(s)");

            // alone on the server this client IS the maintainer: its self-generated keys must
            // still reach the ui (locally delivered, queued through the pre-burst window)
            let keys_frame = frames.find(function(f)
            {
                return f.message && f.message.type === "direct_chat_message"
                    && f.message.some_json && f.message.some_json.type === "channel_keys_from_maintainer";
            });

            t.check("the ui received the self-maintainer channel keys", keys_frame != null,
                "no keys frame among " + frames.length + " frame(s)");

            t.finish();
        }, 2000);
    });
});
