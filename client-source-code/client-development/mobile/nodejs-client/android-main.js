// starts the embedded Node client in the Android foreground service

// keep one rotated log when native settings enable file logging

let fs = require("fs");
let path = require("path");
let log_file = path.join(__dirname, "node-log.txt");
let LOG_CAP_BYTES = 1024 * 1024;

// native settings control file logging
let is_file_logging_enabled = false;

global.lemonchat_set_file_logging = function(is_enabled)
{
    is_file_logging_enabled = (is_enabled === true);
    console.log("file logging " + (is_file_logging_enabled ? "enabled" : "disabled"));
};

/**
 * @brief appends a log line and rotates the file at its size limit
 *
 * @param line formatted log line
 */
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
            }
            catch (no_old)
            {
            }
            fs.renameSync(log_file, log_file + ".old");
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
        tee_to_file(new Date().toISOString() + " [" + name + "] " + Array.prototype.map.call(arguments, String).join(" "));
    };
});

// keep the embedded runtime alive after an uncaught callback failure

process.on("uncaughtException", function(error)
{
    console.error("UNCAUGHT: background runtime callback failed");
});

process.on("unhandledRejection", function(reason)
{
    console.error("UNHANDLED REJECTION: background runtime operation failed");
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

// start native control before opening the page listener
let native_bridge = require("./android-bridge-client.js").start(bundle);
require("./loopback-ui-server.js").start(bundle, function(port, token)
{
    console.log("loopback ui ready on port " + port);
    native_bridge.set_loopback_info({ loopback_port: port, loopback_token: token });
});

console.log("android-main alive, waiting for the bridge to hand over server details");

// keep the service's Node thread active
setInterval(function()
{
}, 60000);
