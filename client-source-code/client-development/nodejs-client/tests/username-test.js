// exercises the connect-time chosen username and the allow_client_renames server switch
// against a running server. one mode per process, because the bundle owns globalThis:
//   node username-test.js hold <name> <seconds> [port]        connect with a chosen name, verify it, stay connected
//   node username-test.js expect-fallback <name> [port]       chosen name is taken, verify the assigned fallback
//   node username-test.js rename-allowed <name> [port]        rename after connect, verify it applies
//   node username-test.js rename-blocked-then-admin <name> <admin_password> [port]
//       verify a user rename is ignored, then become admin and verify the same rename works

let t = require("./test-helpers.js");
t.set_name("USERNAME TEST");

let mode = process.argv[2];
let chosen_name = process.argv[3] || "lemon_tester";
let extra = process.argv[4];
let port = process.argv[5] || process.argv[4];

if (mode === "hold" || mode === "rename-blocked-then-admin") { port = process.argv[5]; }
else { port = process.argv[4]; }
port = port || "2111";

let bundle = require("../load-bundle.js")();

function own_username()
{
    let state = bundle.read_state();

    for (let i = 0; i < state.g_client_list.length; i++)
    {
        if (state.g_client_list[i].client_id == state.local_client_id)
        {
            return state.g_client_list[i].username;
        }
    }
    return null;
}

function own_has_admin_tag()
{
    let state = bundle.read_state();

    for (let i = 0; i < state.g_client_list.length; i++)
    {
        if (state.g_client_list[i].client_id == state.local_client_id)
        {
            let tag_ids = state.g_client_list[i].tag_ids || [];
            return tag_ids.indexOf(0) != -1;
        }
    }
    return false;
}

if (mode === "hold" || mode === "expect-fallback")
{
    bundle.node_set_chosen_username(chosen_name);
}

bundle.init_node_runtime();
bundle.android_js_bridge.accept_current_settings_from_android(
    t.android_settings({ host: "127.0.0.1", port: port }));

t.wait_until("logged in", function()
{
    return bundle.read_state().g_is_client_list_retrieved === true && own_username() != null;
}, 20000, function()
{
    if (mode === "hold")
    {
        t.check("connected with the chosen username", own_username() === chosen_name, "got: " + own_username());
        let seconds = parseInt(extra) || 8;
        setTimeout(function() { t.finish(); }, seconds * 1000);
    }
    else if (mode === "expect-fallback")
    {
        t.check("taken chosen name fell back to an assigned one", own_username() !== chosen_name, "got: " + own_username());
        t.finish();
    }
    else if (mode === "rename-allowed")
    {
        // the login itself counts as an action, so an instant rename lands in the spam
        // cooldown and is dropped; a short settle keeps the test honest
        setTimeout(function()
        {
            bundle.client_msg.send_change_client_username_request(chosen_name, bundle.read_state().local_client_id);

            t.wait_until("rename applied", function() { return own_username() === chosen_name; }, 6000, function()
            {
                t.check("rename applied while renames are allowed", own_username() === chosen_name, "got: " + own_username());
                t.finish();
            });
        }, 2000);
    }
    else if (mode === "rename-blocked-then-admin")
    {
        let name_before = own_username();
        setTimeout(function()
        {
        bundle.client_msg.send_change_client_username_request(chosen_name, bundle.read_state().local_client_id);

        // the rename must be silently ignored, so nothing arrives - give it a fair window
        setTimeout(function()
        {
            t.check("user rename ignored while renames are off", own_username() === name_before, "got: " + own_username());

            bundle.node_forward_raw_request(JSON.stringify({ message: { type: "admin_password", value: extra } }));

            t.wait_until("admin tag granted", own_has_admin_tag, 6000, function()
            {
                // same settle as above, so the admin rename does not die in the cooldown either
                setTimeout(function()
                {
                    bundle.client_msg.send_change_client_username_request(chosen_name, bundle.read_state().local_client_id);

                    t.wait_until("admin rename applied", function() { return own_username() === chosen_name; }, 6000, function()
                    {
                        t.check("admin rename bypassed the switch", own_username() === chosen_name, "got: " + own_username());
                        t.finish();
                    });
                }, 2000);
            });
        }, 4000);
        }, 2000);
    }
    else
    {
        console.log("unknown mode: " + mode);
        process.exit(1);
    }
});
