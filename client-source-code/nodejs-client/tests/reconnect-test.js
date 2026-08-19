// the recovery gate: login, kill the server, node must notice and reconnect on its own once the
// server returns. whatever kills a connection in the field, THIS machinery is what must not fail.
//   set LEMONCHAT_TEST_SERVER=<path to chat-server.exe>, then: node reconnect-test.js

let t = require("./test-helpers.js");
t.set_name("RECONNECT TEST");

let child_process = require("child_process");
let path = require("path");

let server_exe = process.env.LEMONCHAT_TEST_SERVER || "";

if (server_exe === "")
{
    console.log("RECONNECT TEST: SKIPPED (set LEMONCHAT_TEST_SERVER to the chat-server exe)");
    process.exit(0);
}

function spawn_server()
{
    let child = child_process.spawn(server_exe, [], { cwd: path.dirname(server_exe), stdio: "ignore" });
    child.on("error", function(spawn_error) { t.check("server spawned", false, String(spawn_error)); t.finish(); });
    return child;
}

let server = spawn_server();

// never leave an orphan holding the port - it poisons every later run
process.on("exit", function() { try { server.kill(); } catch (e) { } });

// the server cold-starts slowly; connecting before it listens wastes a whole 30s retry cycle
setTimeout(function()
{
    t.start_connected_bundle(function(bundle)
    {
        t.check("logged in against the live server", true);

        console.log("  ...  killing the server");
        server.kill();

        // detection: instant on a clean close, <=35s via the loss detector, +15s watchdog slack
        t.wait_until("node noticed the dead connection", function()
        {
            return bundle.read_state().g_is_authenticated === false;
        }, 60000, function()
        {
            t.check("g_is_authenticated dropped", true);

            console.log("  ...  restarting the server");
            server = spawn_server();

            // the reconnect ticker fires every 30s; allow one full cycle plus the handshake
            t.wait_until("node reconnected by itself", function()
            {
                return bundle.read_state().g_is_authenticated === true
                    && bundle.read_state().g_local_username.length > 0;
            }, 90000, function()
            {
                t.check("logged back in with no outside help", true);
                server.kill();
                setTimeout(t.finish, 300);
            });
        });
    });
}, 20000);
