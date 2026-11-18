// this is the first file that Node.js runs on the phone.
// Node.js is a program that runs JavaScript outside a web browser. the Android app starts it in its background
// service, the part of the app that keeps running while the app window is closed.
// this file loads the chat client, connects it to the Java part of the app and starts the small server inside the
// phone that the page in the app window connects to

// file logging. when it is turned on in the app settings, every console message is also written to node-log.txt in
// this folder. when the file grows over 1 MB, it is renamed to node-log.txt.old and a new file is started

let fs = require("fs");
let path = require("path");
let log_file = path.join(__dirname, "node-log.txt");
let LOG_CAP_BYTES = 1024 * 1024;
let is_file_logging_enabled = false;

// the Java part of the app turns file logging on or off with this function, through android-bridge-client.js
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

        // start a new file
        if (fs.statSync(log_file).size > LOG_CAP_BYTES)
        {
            try
            {
                fs.unlinkSync(log_file + ".old");
            }
            catch (no_old)
            {}
            fs.renameSync(log_file, log_file + ".old");
        }
    }
    catch (tee_error)
    {

    }
}

// every console.log, console.warn and console.error call also goes to the log file
["log", "warn", "error"].forEach(function(name)
{
    let original = console[name];

    console[name] = function()
    {
        original.apply(console, arguments);
        tee_to_file(new Date().toISOString() + " [" + name + "] " + Array.prototype.map.call(arguments, String).join(" "));
    };
});

// an error that no code handles would end Node.js, and the chat client with it.
// these two handlers only write the error to the log, so the client keeps running

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

// bundle.js is the chat client. it holds the JavaScript files of the browser page, joined into one file by
// build-node.py, without the parts that draw the page. load-bundle.js runs it and returns its functions
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

// start the client with its helper threads, its regular checks of the server connection and its identity keys
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

// android-bridge-client.js connects to the Java part of the app through a network connection inside the phone
// (127.0.0.1). through it the Java part says which server to join, and the client reports its state.
// loopback-ui-server.js starts the small server inside the phone that the page in the app window connects to. when it
// is ready, the Java part receives its port and a random password (a token), so that only this page can connect.
// the connection to the Java part starts first, because the port and the password are sent through it
let native_bridge = require("./android-bridge-client.js").start(bundle);
require("./loopback-ui-server.js").start(bundle, function(port, token)
{
    console.log("loopback ui ready on port " + port);
    native_bridge.set_loopback_info({ loopback_port: port, loopback_token: token });
});

console.log("android-main alive, waiting for the bridge to hand over server details");

// Node.js stops when it has nothing left to do. this empty timer keeps it running
setInterval(function() {}, 60000);
