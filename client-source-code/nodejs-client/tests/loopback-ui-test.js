// the loopback replay: node logs into a real server, then a ui client attaches and must receive a
// well-formed login burst
//   node loopback-ui-test.js       (a server must be listening on 1111)

let t = require("./test-helpers.js");
t.set_name("LOOPBACK UI TEST");

let WebSocket = require("../mini-ws.js").WebSocketClient;

t.start_connected_bundle(function(bundle)
{
    console.log("node is logged in, starting loopback server");

    require("../loopback-ui-server.js").start(bundle, function(port, token)
    {
        let frames = [];
        let ui = new WebSocket("ws://127.0.0.1:" + port);

        ui.on("open", function() { ui.send(token); });
        ui.on("message", function(data)
        {
            try { frames.push(JSON.parse(data.toString("utf8"))); } catch (e) { }
        });

        setTimeout(function()
        {
            t.check("burst arrived", frames.length >= 2, "got " + frames.length + " frame(s)");

            let channel_list = frames.find(function(f) { return f.message && f.message.type === "channel_list"; });
            let client_list = frames.find(function(f) { return f.message && f.message.type === "client_list"; });

            t.check("channel_list frame present", channel_list != null);
            t.check("client_list frame present", client_list != null);

            if (channel_list != null)
            {
                t.check("channels carry the root channel",
                    channel_list.message.channels.some(function(c) { return c.is_root_channel === true; }));
            }

            if (client_list != null)
            {
                t.check("client_list carries local_username",
                    client_list.message.local_username === bundle.read_state().g_local_username);
                t.check("clients array is populated", client_list.message.clients.length >= 1);
            }

            t.check("channel_list precedes client_list",
                frames.indexOf(channel_list) !== -1 && frames.indexOf(channel_list) < frames.indexOf(client_list));

            // a wrong token must get nothing
            let intruder_frames = 0;
            let intruder = new WebSocket("ws://127.0.0.1:" + port);

            intruder.on("open", function() { intruder.send("wrong-token"); });
            intruder.on("message", function() { intruder_frames++; });
            intruder.on("error", function() {});

            setTimeout(function()
            {
                t.check("wrong token gets nothing", intruder_frames === 0, intruder_frames + " frame(s) leaked");
                t.finish();
            }, 1500);
        }, 2000);
    });
});
