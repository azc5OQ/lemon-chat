// one malformed server message must not kill the runtime - node cannot be restarted once dead
//   node crash-test.js

let t = require("./test-helpers.js");
t.set_name("CRASH TEST");

let bundle = require("../load-bundle.js")();

bundle.init_node_runtime();

let callback_calls = [];

bundle.set_on_message_processed(function(message_type, had_error)
{
    callback_calls.push({ type: message_type, had_error: had_error });
});

// a harmless message, then one that throws in its handler (real type, missing payload)
postMessage({ type: "log", value: "crash-test harmless message" });
postMessage({ type: "data_processing_worker__current_channel_active_microphone_usage_from_server" });

setTimeout(function()
{
    t.check("process survived the poisoned message", true);

    // filter by type, not count: background traffic (the keypair round trip) also fires the callback
    let harmless = callback_calls.filter(function(c) { return c.type === "log"; });
    let poisoned = callback_calls.filter(function(c)
    {
        return c.type === "data_processing_worker__current_channel_active_microphone_usage_from_server";
    });

    t.check("callback fired for the harmless message", harmless.length === 1, "fired " + harmless.length);
    t.check("harmless message reported clean", harmless.length === 1 && harmless[0].had_error === false);
    t.check("callback fired for the poisoned message", poisoned.length === 1, "fired " + poisoned.length);
    t.check("poisoned message reported had_error", poisoned.length === 1 && poisoned[0].had_error === true);

    let alive_threw = null;

    try { bundle.server_msg.process_image_sent_status_from_server({ message: {} }); }
    catch (error) { alive_threw = error; }

    t.check("handlers still work after containment", alive_threw === null,
        alive_threw != null ? alive_threw.message : "");

    t.finish();
}, 500);
