// live monitor: connects and reports every state change a notification would key off.
// runs until killed - send the client a message from elsewhere and watch.
//   node listen-test.js [host] [port]

let t = require("./test-helpers.js");

let bundle = require("../load-bundle.js")();

bundle.init_node_runtime();
bundle.android_js_bridge.accept_current_settings_from_android(t.android_settings({
    host: process.argv[2] || "127.0.0.1",
    port: process.argv[3] || "1111"
}));

let previous = {};

function stamp()
{
    return new Date().toISOString().substring(11, 23);
}

function report(key, value, prefix)
{
    if (previous[key] !== value)
    {
        previous[key] = value;
        console.log("[" + stamp() + "] " + prefix + value);
    }
}

setInterval(function()
{
    let state = bundle.read_state();

    if (state.g_local_username && previous.logged_in == null)
    {
        previous.logged_in = true;
        console.log("[" + stamp() + "] LOGGED IN as \"" + state.g_local_username + "\"");
    }

    report("members", state.g_client_list.map(function(c) { return c.client_id + ":" + c.username; }).join(", "), "MEMBERS: ");

    let unread = state.g_client_list
        .filter(function(c) { return (c.unread_count || 0) > 0; })
        .map(function(c) { return c.username + "=" + c.unread_count; })
        .join(", ");

    if (unread.length > 0) { report("unread", unread, "*** UNREAD: "); }

    report("senders", state.g_chat_context_array
        .map(function(c) { return c.chat_context_id + "<-" + (c.last_known_message_sender_username || "-"); })
        .join(", "), "CONTEXTS: ");
}, 250);

console.log("listening - send this client a message");
