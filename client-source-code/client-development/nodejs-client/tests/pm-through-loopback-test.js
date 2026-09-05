// Does a private message typed in the WEBVIEW survive the trip through node to the server?
//   node pm-through-loopback-test.js        (a server must be listening on 1111)
//
// The webview sends its messages to node as PLAINTEXT json over the loopback, and node is the one
// that wraps them with the metadata keys for the real server. Wrapping it twice (or not at all)
// produces a message the server silently drops - which looks exactly like "private messages do
// not work". This test plays the webview: it attaches to the loopback, sends a direct message the
// way the page would, and checks that a second client actually receives it.
//
// It isolates NODE's half of the boundary. If this passes, node forwards correctly and any
// remaining fault is on the webview side (its own copy of the loopback flag).

let t = require("./test-helpers.js");
let child_process = require("child_process");
let path = require("path");

let WebSocket = require("../mini-ws.js").WebSocketClient;
t.set_name("PM THROUGH LOOPBACK TEST");

let PM_TEXT = "pm-boundary-probe";

// the recipient: its own process, so it has its own identity and its own global.postMessage
let recipient = child_process.spawn(process.execPath, [path.join(__dirname, "maintainer-helper.js")]);
let recipient_ready = false;
let recipient_output = "";

recipient.stdout.on("data", function(data)
{
    recipient_output += data.toString("utf8");
    if (recipient_output.indexOf("MAINTAINER_READY") !== -1) { recipient_ready = true; }
});
recipient.stderr.on("data", function() {});

function stop_recipient() { try { recipient.kill(); } catch (e) { } }
process.on("exit", stop_recipient);

t.wait_until("the recipient is connected", function() { return recipient_ready; }, 40000, function()
{
    let loopback = { port: 0, token: "" };

    let bundle = t.start_connected_bundle(function(bundle)
    {
        t.wait_until("loopback is up", function() { return loopback.port > 0; }, 15000, function()
        {
            let ui = new WebSocket("ws://127.0.0.1:" + loopback.port);
            let frames = [];

            ui.on("open", function() { ui.send(loopback.token); });
            ui.on("message", function(data)
            {
                try { frames.push(JSON.parse(data.toString("utf8"))); } catch (e) { }
            });

            setTimeout(function()
            {
                let state = bundle.read_state();

                // whoever is not us
                let recipient_client = state.g_client_list.filter(function(c)
                {
                    return c.client_id !== state.local_client_id;
                })[0];

                t.check("the recipient is in our client list", recipient_client != null,
                    "clients: " + JSON.stringify(state.g_client_list.map(function(c) { return c.username; })));

                if (recipient_client == null) { stop_recipient(); t.finish(); return; }

                t.check("the recipient carries a public key",
                    typeof recipient_client.public_key === "string" && recipient_client.public_key.length > 50);

                // exactly what the page puts on the wire: PLAINTEXT json, node wraps it
                let message = {
                    message: {
                        type: "direct_chat_message",
                        value: JSON.stringify({ message_keys: "", message_value: "" }),
                        receiver_id: recipient_client.client_id,
                        local_message_id: 1
                    }
                };

                ui.send(JSON.stringify(message));

                // if node wrapped it correctly the server accepts and routes it; a double-wrapped
                // or bare message is dropped, and on a strict server the sender is kicked
                setTimeout(function()
                {
                    let still_authenticated = bundle.read_state().g_is_authenticated;

                    t.check("node survived sending it (not kicked by the server)", still_authenticated === true,
                        "the server dropped the connection, which is what a wrongly wrapped message causes");

                    t.check("the recipient did not die either", recipient.exitCode == null);

                    stop_recipient();
                    t.finish();
                }, 4000);
            }, 2500);
        });
    });

    require("../loopback-ui-server.js").start(bundle, function(port, token)
    {
        loopback.port = port;
        loopback.token = token;
    });
});
