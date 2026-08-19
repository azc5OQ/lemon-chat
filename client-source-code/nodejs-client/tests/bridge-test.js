// Plays the java side of the bridge against the REAL android-main.js, run as a child process,
// against a live server on 127.0.0.1:1111. Proves the whole android flow without a device:
// settings command -> node connects and logs in -> a second client joining is reported ->
// disconnect command parks the socket and it STAYS parked.
//
//   node bridge-test.js        (a server must be listening on 1111)

let t = require("./test-helpers.js");
let net = require("net");
let fs = require("fs");
let path = require("path");
let child_process = require("child_process");

let failures = 0;
let child = null;
let second_client = null;
let bridge_connection = null;
let events = [];
let child_output_tail = [];

function check(label, condition, detail)
{
    if (condition)
    {
        console.log("  ok    " + label);
    }
    else
    {
        failures++;
        console.log("  FAIL  " + label + (detail != null ? "  ->  " + detail : ""));
    }
}

function finish()
{
    if (child != null) { child.kill(); }
    if (second_client != null) { second_client.kill(); }

    console.log("");
    console.log("-".repeat(60));
    console.log(failures === 0 ? "BRIDGE TEST: PASS" : "BRIDGE TEST: FAIL (" + failures + ")");

    if (failures > 0)
    {
        console.log("");
        console.log("last child output:");
        child_output_tail.slice(-80).forEach(function(line) { console.log("  child| " + line); });
    }

    process.exit(failures === 0 ? 0 : 1);
}

// wait until an event matching the predicate arrives (also checks those already received)
function wait_for_event(label, predicate, timeout_ms, next)
{
    let existing = events.find(predicate);

    if (existing != null)
    {
        check(label, true);
        next(existing);
        return;
    }

    let started = Date.now();

    let poll = setInterval(function()
    {
        let match = events.find(predicate);

        if (match != null)
        {
            clearInterval(poll);
            check(label, true);
            next(match);
        }
        else if (Date.now() - started > timeout_ms)
        {
            clearInterval(poll);
            check(label, false, "timed out after " + timeout_ms + "ms; got: "
                + JSON.stringify(events.slice(-5)));
            finish();
        }
    }, 200);
}

let token = "bridge-test-" + Math.random().toString(36).substring(2);

let server = net.createServer(function(connection)
{
    let authed = false;
    let buffer = "";

    connection.on("data", function(data)
    {
        buffer += data.toString("utf8");

        let newline_index;
        while ((newline_index = buffer.indexOf("\n")) !== -1)
        {
            let line = buffer.substring(0, newline_index).trim();
            buffer = buffer.substring(newline_index + 1);

            if (authed === false)
            {
                check("node authenticated with the token", line === token, "got: " + line);
                authed = true;
                bridge_connection = connection;
                continue;
            }

            try { events.push(JSON.parse(line)); } catch (e) { }
        }
    });

    connection.on("error", function() {});
});

server.listen(0, "127.0.0.1", function()
{
    let port = server.address().port;

    fs.writeFileSync(path.join(__dirname, "..", "bridge.json"),
        JSON.stringify({ port: port, token: token }));

    console.log("fake java listening on " + port + ", starting android-main.js");

    child = child_process.spawn(process.execPath, ["android-main.js"], { cwd: path.join(__dirname, "..") });

    child.stdout.on("data", function(d) { d.toString().split("\n").forEach(function(l) { if (l) child_output_tail.push(l); }); });
    child.stderr.on("data", function(d) { d.toString().split("\n").forEach(function(l) { if (l) child_output_tail.push(l); }); });

    // 1. node should dial in and authenticate, then get settings and log in
    wait_for_event("initial status arrived", function(e) { return e.type === "status"; }, 15000, function()
    {
        bridge_connection.write(JSON.stringify({
            type: "settings",
            json: t.android_settings()
        }) + "\n");

        wait_for_event("logged in after settings command",
            function(e) { return e.type === "status" && e.logged_in === true; }, 30000, function()
        {
            // 2. a second client joins; the bridge must report the member change
            console.log("  ...  spawning a second client");
            second_client = child_process.spawn(process.execPath, ["tests/connect-test.js"],
                { cwd: path.join(__dirname, ".."), env: Object.assign({}, process.env, { LEMONCHAT_LINGER_MS: "6000" }) });

            second_client.stdout.on("data", function(d) { d.toString().split("\n").forEach(function(l) { if (l) child_output_tail.push("peer: " + l); }); });
            second_client.stderr.on("data", function(d) { d.toString().split("\n").forEach(function(l) { if (l) child_output_tail.push("peer: " + l); }); });

            wait_for_event("second client reported via member_change",
                function(e) { return e.type === "member_change" && e.members.split(",").length >= 2; }, 30000, function()
            {
                // 3. disconnect must close the socket and KEEP it closed past the 30s reconnect tick
                bridge_connection.write(JSON.stringify({ type: "disconnect" }) + "\n");

                wait_for_event("disconnected after disconnect command",
                    function(e) { return e.type === "status" && e.connected === false; }, 15000, function()
                {
                    console.log("  ...  watching 40s to prove the reconnect ticker stays parked");
                    let events_at_disconnect = events.length;

                    setTimeout(function()
                    {
                        let reconnected = events.slice(events_at_disconnect).some(function(e)
                        {
                            return e.type === "status" && e.connected === true;
                        });

                        check("still parked 40s later (reconnect ticker suppressed)", reconnected === false,
                            JSON.stringify(events.slice(events_at_disconnect)));
                        finish();
                    }, 40000);
                });
            });
        });
    });
});
