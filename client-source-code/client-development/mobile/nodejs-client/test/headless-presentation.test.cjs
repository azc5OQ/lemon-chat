const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const {spawnSync} = require("node:child_process");

const directory = fs.mkdtempSync(path.join(os.tmpdir(), "lemon-headless-presentation-"));
const output = path.join(directory, "bundle.js");
const built = spawnSync("python", ["-c", [
    "import importlib.util,sys",
    "spec=importlib.util.spec_from_file_location('builder',sys.argv[1])",
    "builder=importlib.util.module_from_spec(spec)",
    "spec.loader.exec_module(builder)",
    "builder.OUT=sys.argv[2]",
    "sys.exit(builder.main())"
].join("\n"), path.resolve(__dirname, "../build-node.py"), output], {encoding: "utf8"});
assert.equal(built.status, 0, built.stderr || built.stdout);
const source = fs.readFileSync(output, "utf8");
fs.unlinkSync(output);
fs.rmdirSync(directory);

test("manual native settings do not start a headless dialog layout timer", function()
{
    const timers = [];
    const errors = [];
    const context =
    {
        process: {}, crypto: crypto.webcrypto, Buffer, TextDecoder, TextEncoder, performance, atob, btoa,
        console:
        {
            log: function()
            {
            },
            warn: function()
            {
            },
            error: function(value)
            {
                errors.push(String(value));
            }
        },
        setTimeout: function(callback, delay)
        {
            timers.push({callback, delay});
            return timers.length;
        },
        clearTimeout: function()
        {
        },
        setInterval: function()
        {
        },
        clearInterval: function()
        {
        }
    };
    context.global = context;
    vm.createContext(context);
    vm.runInContext(source, context, {timeout: 10000});
    context.what.init_node_runtime();
    context.what.android_js_bridge.accept_current_settings_from_android(JSON.stringify(
    {
        host: "127.0.0.1", port: 29111, metadata_keys: [], app_mode: "advanced",
        is_autoconnect_enabled: false, is_audio_effect_enabled: false, is_microphone_always_on: false
    }));
    const layoutTimers = timers.filter(function(timer)
    {
        return timer.callback.name === "connection__apply_connect_window_mode";
    });
    assert.equal(layoutTimers.length, 0, "Node must not repeatedly wait for browser dimensions");
    assert.deepEqual(errors, []);
});
