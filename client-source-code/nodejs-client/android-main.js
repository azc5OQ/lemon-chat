// Entry point for the embedded node runtime in the android foreground service. Started by
// NodeRuntime.java via node::Start(); stdout/stderr land in logcat under "lemonchat-node".
// Java hands over server details through the bridge; node then owns the one server connection.

// debug log: everything node prints also lands in node-log.txt next to the bundle (capped, one
// .old generation), so a whole night survives logcat's buffer rotation
let fs = require("fs");
let path = require("path");
let log_file = path.join(__dirname, "node-log.txt");
let LOG_CAP_BYTES = 1024 * 1024;

// whether we write the log file at all. java tells us, so one setting covers the whole app
let is_file_logging_enabled = true;

global.lemonchat_set_file_logging = function(is_enabled)
{
    is_file_logging_enabled = (is_enabled === true);
    console.log("file logging " + (is_file_logging_enabled ? "enabled" : "disabled"));
};

function tee_to_file(line)
{
    if (is_file_logging_enabled === false)
    {
        return;
    }

    try
    {
        fs.appendFileSync(log_file, line + "\n");

        if (fs.statSync(log_file).size > LOG_CAP_BYTES)
        {
            try
            {
                fs.unlinkSync(log_file + ".old");
            } catch (no_old) 
            { 
                fs.renameSync(log_file, log_file + ".old");
            }
        }
    }
    catch (tee_error)
    {
        
    }
}

["log", "warn", "error"].forEach(function(name)
{
    let original = console[name];

    console[name] = function()
    {
        original.apply(console, arguments);
        tee_to_file(new Date().toISOString() + " [" + name + "] "
            + Array.prototype.map.call(arguments, String).join(" "));
    };
});

// last-resort containment: node::Start works once per process, so an uncaught throw would leave a
// permanently dead runtime. dispatch_safely (main.js) contains per-message throws; this catches
// the rest. log and keep the loop alive.
process.on("uncaughtException", function(error)
{
    console.error("UNCAUGHT: " + (error && error.stack ? error.stack : error));
});

process.on("unhandledRejection", function(reason)
{
    console.error("UNHANDLED REJECTION: " + (reason && reason.stack ? reason.stack : reason));
});

console.log("android-main starting");
console.log("node " + process.version + " on " + process.platform + "/" + process.arch);
console.log("cwd " + process.cwd());
console.log("__dirname " + __dirname);

let bundle = null;

try
{
    bundle = require("./load-bundle.js")();
}
catch (error)
{
    console.error("FAILED to load the client bundle: " + (error && error.stack ? error.stack : error));
    process.exit(1);
}

console.log("bundle loaded, exports: " + Object.keys(bundle).join(", "));
console.log("WebSocket is " + (typeof WebSocket));

try
{
    bundle.init_node_runtime();
    console.log("node runtime initialised (loopback workers + log sink + keypair requested)");
}
catch (error)
{
    console.error("FAILED to init the runtime: " + (error && error.stack ? error.stack : error));
    process.exit(1);
}

// NO synthetic selftest here: feeding a fake channel_list sets g_is_channel_list_retrieved, and the
// real server's channel list would then be rejected as a duplicate. real traffic is the test now.

// loopback first, so the bridge can announce it to java in its hello
require("./loopback-ui-server.js").start(bundle, function(port, token)
{
    console.log("loopback ui ready on port " + port);
    require("./android-bridge-client.js").start(bundle, { loopback_port: port, loopback_token: token });
});

console.log("android-main alive, waiting for the bridge to hand over server details");

// hold the event loop open so node does not exit and take the service's worker thread with it
setInterval(function()
{
}, 60000);
