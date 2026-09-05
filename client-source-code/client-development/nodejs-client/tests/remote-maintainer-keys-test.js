// channel keys when SOMEONE ELSE is the maintainer, delivered to a ui that attaches late.
//   node remote-maintainer-keys-test.js      (a server must be listening on 1111)
//
// this is the android case: node logs in and gets the keys within ~100ms, the webview attaches
// seconds later and so can only ever see what the burst replays. if the keys frame was not cached,
// the webview never learns the channel keys and channel chat stays unreadable.
//
// on the wire the marker is AES-encrypted inside message_value, so a remote maintainer's keys are
// indistinguishable from a private message - which is why the cache used to miss them. node now
// decrypts before forwarding, so the ui sees a readable payload and needs no key of its own.

let t = require("./test-helpers.js");
let child_process = require("child_process");
let path = require("path");

let WebSocket = require("../mini-ws.js").WebSocketClient;
t.set_name("REMOTE MAINTAINER KEYS TEST");

let maintainer = child_process.spawn(process.execPath, [path.join(__dirname, "maintainer-helper.js")]);
let maintainer_ready = false;

maintainer.stdout.on("data", function(data)
{
    if (data.toString("utf8").indexOf("MAINTAINER_READY") !== -1) { maintainer_ready = true; }
});
maintainer.stderr.on("data", function() {});

function stop_maintainer()
{
    try { maintainer.kill(); } catch (e) { }
}

// the helper must be in the channel BEFORE we join, so that it - not us - is the maintainer
t.wait_until("a remote maintainer is connected", function() { return maintainer_ready; }, 40000, function()
{
    console.log("remote maintainer is up, connecting the node under test");

    let loopback = { port: 0, token: "" };

    let bundle = t.start_connected_bundle(function(bundle)
    {
        // node is idle here, with no ui - an idle client still receives the maintainer's keys,
        // which is the whole reason the phone can be reached while it sits in the background
        t.wait_until("node received channel keys from the remote maintainer", function()
        {
            return bundle.read_state().current_channel_keys != null && loopback.port > 0;
        }, 30000, function()
        {
            t.check("node received channel keys from the remote maintainer", true);

            // only NOW attach a ui, the way the webview does - well after the keys arrived
            {
                let frames = [];
                let ui = new WebSocket("ws://127.0.0.1:" + loopback.port);

                ui.on("open", function() { ui.send(loopback.token); });
                ui.on("message", function(data)
                {
                    try { frames.push(JSON.parse(data.toString("utf8"))); } catch (e) { }
                });

                setTimeout(function()
                {
                    t.check("burst arrived", frames.length >= 2, "got " + frames.length + " frame(s)");

                    let keys_frame = frames.find(function(f)
                    {
                        return f.message && f.message.type === "direct_chat_message";
                    });

                    t.check("burst replays a keys frame to the late ui", keys_frame != null,
                        "no direct_chat_message in the burst, so the ui never learns the channel keys");

                    if (keys_frame != null)
                    {
                        // node decrypts, so the ui gets a usable payload and holds no private key
                        t.check("the keys frame carries the decrypted keys payload",
                            keys_frame.message.some_json != null
                            && keys_frame.message.some_json.type === "channel_keys_from_maintainer",
                            "some_json = " + JSON.stringify(keys_frame.message.some_json));

                        t.check("no encrypted payload is left for the ui to decrypt",
                            keys_frame.message.value == null);

                        // it must be the REMOTE maintainer's message, not our own echo
                        t.check("the keys frame came from the remote maintainer",
                            keys_frame.message.sender_id !== bundle.read_state().local_client_id,
                            "sender_id " + keys_frame.message.sender_id + " is us, expected the remote maintainer");
                    }

                    stop_maintainer();
                    t.finish();
                }, 2500);
            }
        });
    });

    // android starts the loopback before node logs in, so the keys listener is armed in time
    require("../loopback-ui-server.js").start(bundle, function(port, token)
    {
        loopback.port = port;
        loopback.token = token;
    });
});

process.on("exit", stop_maintainer);
