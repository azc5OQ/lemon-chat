// Replays the call-accept race headless: accept into root, ui appears, then the screen "blinks
// off" one second later. Before the presence grace this idled the user right out of the call.
//   node call-accept-grace-probe.js            (a keyless server on 1111)
// Expected: "idle deferred, inside the call-accept presence grace", NO "going idle" inside the
// grace, then ONE "going idle" at the ~10s re-check (the ui really is gone by then).
// A second run with the ui returning at +3s must never go idle at all.
let t = require("./test-helpers.js");
t.set_name("CALL ACCEPT GRACE PROBE");

let with_return = (process.argv[2] === "return");
let saw_deferred = false;
let idle_timestamps = [];
let started_at = 0;

let original_log = console.log;
console.log = function()
{
    let line = Array.prototype.slice.call(arguments).join(" ");
    if (line.indexOf("idle deferred, inside the call-accept presence grace") !== -1) { saw_deferred = true; }
    if (line.indexOf("connect-path: going idle") !== -1 && started_at > 0) { idle_timestamps.push(Date.now() - started_at); }
    original_log.apply(console, arguments);
};

t.start_connected_bundle(function(bundle)
{
    // the accept: java tells node to come from idle into the caller's channel (root here)
    bundle.android_js_bridge.send_come_from_idle_mode_request_android(0);

    // java foregrounds the app; a moment later the screen blinks off
    bundle.node_set_ui_attached(true, true);
    started_at = Date.now();

    setTimeout(function() { bundle.node_set_ui_attached(false, true); }, 1000);

    if (with_return)
    {
        setTimeout(function() { bundle.node_set_ui_attached(true, true); }, 3000);
    }

    setTimeout(function()
    {
        let verdict;
        if (with_return)
        {
            verdict = (saw_deferred && idle_timestamps.length === 0) ? "PASS" : "FAIL";
            original_log("PROBE " + verdict + " (return case): deferred=" + saw_deferred + " idle_events=" + JSON.stringify(idle_timestamps));
        }
        else
        {
            let idled_late = (idle_timestamps.length === 1 && idle_timestamps[0] > 4000);
            verdict = (saw_deferred && idled_late) ? "PASS" : "FAIL";
            original_log("PROBE " + verdict + " (gone case): deferred=" + saw_deferred + " idle_events_ms=" + JSON.stringify(idle_timestamps));
        }
        process.exit(verdict === "PASS" ? 0 : 1);
    }, 9000);
});

setInterval(function() {}, 60000);
