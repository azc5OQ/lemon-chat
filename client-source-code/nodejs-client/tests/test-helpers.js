// shared plumbing for every test in this directory
let failures = 0;
let passes = 0;
let test_name = "TEST";

function set_name(name)
{
    test_name = name;
}

function check(label, condition, detail)
{
    if (condition)
    {
        passes++;
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
    console.log("");
    console.log("-".repeat(60));
    console.log(failures === 0 ? test_name + ": PASS (" + passes + " checks)" : test_name + ": FAIL (" + failures + ")");
    process.exit(failures === 0 ? 0 : 1);
}

// the settings json java would hand over; override fields per test
function android_settings(overrides)
{
    let settings = {
        app_mode: "", host: "127.0.0.1", port: "1111", metadata_keys: [],
        is_autoconnect_enabled: true, is_audio_effect_enabled: false,
        is_app_log_enabled: false, is_microphone_always_on: false
    };

    for (let key in (overrides || {})) { settings[key] = overrides[key]; }

    return JSON.stringify(settings);
}

// polls until the predicate is true, then calls on_ready; fails the test on timeout
function wait_until(label, predicate, timeout_ms, on_ready)
{
    let started = Date.now();

    let poll = setInterval(function()
    {
        if (predicate())
        {
            clearInterval(poll);
            on_ready();
        }
        else if (Date.now() - started > timeout_ms)
        {
            clearInterval(poll);
            check(label, false, "timed out after " + timeout_ms + "ms");
            finish();
        }
    }, 200);
}

// loads the bundle, stands up the runtime, connects, and waits for a completed login
function start_connected_bundle(on_logged_in)
{
    let bundle = require("../load-bundle.js")();

    bundle.init_node_runtime();
    bundle.android_js_bridge.accept_current_settings_from_android(android_settings());

    wait_until("logged in", function()
    {
        let state = bundle.read_state();
        return state.g_is_authenticated === true && state.g_local_username.length > 0;
    }, 30000, function() { on_logged_in(bundle); });

    return bundle;
}

module.exports = {
    set_name: set_name,
    check: check,
    finish: finish,
    android_settings: android_settings,
    wait_until: wait_until,
    start_connected_bundle: start_connected_bundle
};
