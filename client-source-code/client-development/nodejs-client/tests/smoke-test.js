// the gate for the headless port: the bundle loads, the protocol table is reachable, and real
// server messages build state with no dom.
//   python ../build-node.py && node smoke-test.js

let t = require("./test-helpers.js");
t.set_name("SMOKE TEST");
let check = t.check;

console.log("loading bundle ...");

let bundle = require("../load-bundle.js")();

console.log("");
console.log("1. the seam");

check("require() returned an object", bundle != null && typeof bundle === "object",
    "got " + typeof bundle);

check("server_msg is reachable", typeof bundle.server_msg === "object");
check("client_msg is reachable", typeof bundle.client_msg === "object");
check("read_state is reachable", typeof bundle.read_state === "function");

console.log("");
console.log("2. the shim is live and the dom is absent");

check("document exists but is a shim", typeof document !== "undefined");
check("no real dom: getElementById returns a non-null dead element",
    document.getElementById("anything") != null);
check("collections stay empty so paint loops do nothing",
    document.getElementsByClassName("anything").length === 0);
check("...but are still indexable, which is what stops the blind c[0] / c[n-1] crashes",
    document.getElementsByClassName("anything")[0] != null);

// the infinite-loop tripwire. messages.js:1735-1738 is
//     var lights = document.getElementsByClassName("imgnotsentyet");
//     while (lights.length) { lights[0].classList.remove("imgnotsentyet"); }
// classList.remove is a no-op here, so if a dead collection ever reports a non-zero length this
// call never returns and the whole runtime hangs. If this test hangs, that is the bug.
let image_status_threw = null;

try
{
    bundle.server_msg.process_image_sent_status_from_server({ message: {} });
}
catch (error)
{
    image_status_threw = error;
}

check("the `while (lights.length)` handler terminates", image_status_threw === null,
    image_status_threw != null ? image_status_threw.message : "");

console.log("");
console.log("3. a real server message builds real state");

// field for field what the server puts on the wire, from base.c:456-468. worth keeping faithful:
// the first version of this test invented `is_temporary` and marked the root with
// parent_channel_id -1, and the tree builder correctly rejected the whole payload. root channels
// are identified by the is_root_channel FLAG, because a uint64 parent id cannot carry -1
// (see channel_tree__get_channels_by_channel_parent_id)
function channel(channel_id, parent_channel_id, is_root_channel, name)
{
    return {
        channel_id: channel_id,
        parent_channel_id: parent_channel_id,
        type: 0,
        is_root_channel: is_root_channel,
        is_using_password: false,
        is_audio_enabled: true,
        is_client_limit_active: false,
        max_client_count: 0,
        has_channel_icon: false,
        channel_icon_id: -1,
        name: name,
        password: "",
        description: ""
    };
}

let channel_list_message = {
    message: {
        channels: [
            // root carries parent -1, as the server sets it (base.c root_channel->parent_channel_id
            // = -1, serialised through an (int64) cast). giving it 0 makes it its own child and the
            // tree builder stalls
            channel(0, -1, true, "root"),
            channel(1, 0, false, "second channel")
        ]
    }
};

let before = bundle.read_state();
check("g_channel_list starts empty", before.g_channel_list.length === 0,
    "length " + before.g_channel_list.length);

try
{
    bundle.server_msg.process_channel_list_from_server(channel_list_message);
}
catch (error)
{
    check("process_channel_list_from_server did not throw", false, error.message);
}

let after = bundle.read_state();

check("g_channel_list now holds both channels", after.g_channel_list.length === 2,
    "length " + after.g_channel_list.length);

if (after.g_channel_list.length === 2)
{
    check("channel names survived", after.g_channel_list[0].name === "root"
        && after.g_channel_list[1].name === "second channel",
        JSON.stringify(after.g_channel_list.map(function(c) { return c.name; })));
    check("channel ids survived", after.g_channel_list[0].channel_id === 0
        && after.g_channel_list[1].channel_id === 1);
}

check("g_is_channel_list_retrieved flipped", after.g_is_channel_list_retrieved === true);

console.log("");
console.log("4. a handler whose state writes sit BELOW a dom write");

// messages.js:946 does `querySelector(...).children[2].innerHTML = ...` one line ABOVE the six
// g_channel_list writes at 947-952. With `children: []` in the shim, children[2] was undefined, the
// assignment threw, and the rename silently never landed. This is the regression test for that.
let channel_edit_message = {
    message: {
        channel_id: 1,
        channel_editor_id: 99,
        channel_name: "renamed channel",
        channel_description: "a description",
        is_using_password: false,
        is_audio_enabled: false,
        is_client_limit_active: true,
        max_client_count: 7
    }
};

let edit_threw = null;

try
{
    bundle.server_msg.process_channel_edit_from_server(channel_edit_message);
}
catch (error)
{
    edit_threw = error;
}

check("process_channel_edit_from_server did not throw", edit_threw === null,
    edit_threw != null ? edit_threw.message : "");

let edited = bundle.read_state().g_channel_list;
let target = edited.filter(function(c) { return c.channel_id === 1; })[0];

check("the edited channel is still in state", target != null);

if (target != null)
{
    check("rename landed", target.name === "renamed channel", "name is " + target.name);
    check("description landed", target.description === "a description");
    check("is_audio_enabled landed", target.is_audio_enabled === false);
    check("max_client_count landed", target.max_client_count === 7);
}

console.log("");
console.log("5. the dom-guard flips: state decides, not markup");

// client payload per server_message.c:352-420
function client(client_id, channel_id, username, alias)
{
    return {
        username: username,
        public_key: "fake-public-key-" + client_id,
        channel_id: channel_id,
        client_id: client_id,
        is_music_bot: false,
        audio_state: 0,
        tag_ids: [],
        is_idle: false,
        alias: alias,
        country_iso_code: "unknown"
    };
}

let client_list_threw = null;

try
{
    bundle.server_msg.process_client_list_from_server({
        message: {
            clients: [client(0, 0, "me", ""), client(5, 0, "someone", "")],
            local_username: "me"
        }
    });
}
catch (error)
{
    client_list_threw = error;
}

check("process_client_list_from_server did not throw", client_list_threw === null,
    client_list_threw != null ? client_list_threw.message : "");
check("g_client_list built", bundle.read_state().g_client_list.length === 2,
    "length " + bundle.read_state().g_client_list.length);

// process_direct_chat_picture_metadata_from_server used to ask the dom whether the pm context
// existed. Headless getElementById never returns null, so the context was never created and the
// g_chat_context_array.push never ran. Calling it TWICE checks both directions of the new guard:
// the context must be created once, and must not be duplicated on the second message.
function pm_context_count(state)
{
    return state.g_chat_context_array.filter(function(c)
    {
        return c.chat_context_id === "chat-context-pm-5";
    }).length;
}

check("no pm context before the first message", pm_context_count(bundle.read_state()) === 0);

let picture_threw = null;

try
{
    bundle.server_msg.process_direct_chat_picture_metadata_from_server({
        message: { sender_id: 5, sender_username: "someone", picture_id: 1 }
    });
}
catch (error)
{
    picture_threw = error;
}

check("process_direct_chat_picture_metadata_from_server did not throw", picture_threw === null,
    picture_threw != null ? picture_threw.message : "");
check("the pm chat context was CREATED from state, with no dom",
    pm_context_count(bundle.read_state()) === 1,
    "count " + pm_context_count(bundle.read_state()));

try
{
    bundle.server_msg.process_direct_chat_picture_metadata_from_server({
        message: { sender_id: 5, sender_username: "someone", picture_id: 2 }
    });
}
catch (error)
{
    check("second picture message did not throw", false, error.message);
}

check("a second message does NOT duplicate the context",
    pm_context_count(bundle.read_state()) === 1,
    "count " + pm_context_count(bundle.read_state()));

console.log("");
console.log("6. state that used to live inside a click handler");

// g_is_microphone_always_on was flipped by UI.activate_continous_audio_broadcast_onclick with
// `= !g_is_microphone_always_on`. Headless the UI proxy ate the call, so an android switch could
// never propagate. And any second writer would have INVERTED it rather than set it, which is why
// the flip had to move out of the handler rather than be duplicated.
//
// autoconnect is off in these payloads on purpose - otherwise this handler tries to open a socket.
function android_settings(is_microphone_always_on)
{
    return JSON.stringify({
        app_mode: "",
        host: "127.0.0.1",
        port: "8443",
        is_autoconnect_enabled: false,
        is_audio_effect_enabled: false,
        is_app_log_enabled: false,
        is_microphone_always_on: is_microphone_always_on
    });
}

function mic_flag()
{
    return bundle.read_state().g_is_microphone_always_on;
}

let settings_threw = null;

try
{
    // first push: the app handing over its saved settings
    bundle.android_js_bridge.accept_current_settings_from_android(android_settings(false));
}
catch (error)
{
    settings_threw = error;
}

check("accept_current_settings_from_android did not throw", settings_threw === null,
    settings_threw != null ? settings_threw.message : "");
check("first push leaves the mic flag off", mic_flag() === false, "flag is " + mic_flag());

// the switch is moved while running. this is the one that did nothing at all before.
bundle.android_js_bridge.accept_current_settings_from_android(android_settings(true));
check("a switch moved to ON now reaches the flag with no ui", mic_flag() === true,
    "flag is " + mic_flag());

// the inversion regression: re-sending the SAME value must not toggle it back
bundle.android_js_bridge.accept_current_settings_from_android(android_settings(true));
check("re-sending ON does not invert it", mic_flag() === true, "flag is " + mic_flag());

bundle.android_js_bridge.accept_current_settings_from_android(android_settings(false));
check("a switch moved to OFF reaches the flag", mic_flag() === false, "flag is " + mic_flag());

// the java-side second entry point is gone, so the settings push above is the only way in

console.log("");
console.log("7. promoting an offline conversation, with no ui to click");

// UI.promote_offline_chat_context used to own this outright: it re-keyed g_chat_context_array
// entries from chat-context-offline-<alias> to chat-context-pm-<id>. Behind the UI proxy none of it
// happened, so a reconnecting aliased contact kept a stale offline-keyed context forever.
//
// read_state() hands back the LIVE arrays, so the test can seed the offline context the way
// process_offline_chat_message_from_server would have.
bundle.read_state().g_chat_context_array.push({
    type: "user",
    chat_context_id: "chat-context-offline-bob",
    last_known_message_sender_username: ""
});

function has_context(id)
{
    return bundle.read_state().g_chat_context_array.some(function(c)
    {
        return c.chat_context_id === id;
    });
}

check("offline context seeded", has_context("chat-context-offline-bob"));

let connect_threw = null;

try
{
    // bob connects, carrying the alias that keys the offline conversation
    bundle.server_msg.process_client_connect_from_server({
        message: {
            client_id: 7,
            username: "bob",
            alias: "bob",
            public_key: "fake-public-key-7",
            channel_id: 0,
            audio_state: 0,
            tag_ids: [],
            country_iso_code: "unknown",
            is_music_bot: false
        }
    });
}
catch (error)
{
    connect_threw = error;
}

check("process_client_connect_from_server did not throw", connect_threw === null,
    connect_threw != null ? connect_threw.message : "");
check("the offline-keyed context is gone", has_context("chat-context-offline-bob") === false);
check("it was re-keyed to the live private chat", has_context("chat-context-pm-7") === true);

console.log("");
console.log("8. unread counts are real state, not innerHTML");

// the count used to live ONLY in the badge's innerHTML: read out with parseInt, incremented,
// written back. With no dom that parsed "" into NaN, and nothing outside the member list could ask
// how many unread messages somebody had - which is exactly what a background service needs in order
// to raise a notification.
//
// section 5 sent TWO direct chat pictures from client 5 while the current context was the channel,
// so both should have counted.
function unread_for(client_id)
{
    let entry = bundle.read_state().g_client_list.filter(function(c)
    {
        return c.client_id === client_id;
    })[0];

    return entry ? entry.unread_count : "no such client";
}

check("client objects carry an unread_count", typeof unread_for(5) === "number",
    "got " + unread_for(5));
check("two private messages counted as 2 unread", unread_for(5) === 2,
    "count is " + unread_for(5));
check("an untouched client stays at 0", unread_for(0) === 0, "count is " + unread_for(0));

// a third message keeps accumulating rather than resetting
bundle.server_msg.process_direct_chat_picture_metadata_from_server({
    message: { sender_id: 5, sender_username: "someone", picture_id: 3 }
});

check("a further message increments to 3", unread_for(5) === 3, "count is " + unread_for(5));

t.finish();
