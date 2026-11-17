"use strict";

// Assemble current source with the production builder into an isolated ignored directory.
// Regression: fresh full bundle must process membership without browser-only presentation modules.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const { spawnSync } = require("node:child_process");
const repo = path.resolve(__dirname, "../../../../..");
const development = path.join(repo, "client-source-code/client-development");
const prefix = path.join(require("node:os").tmpdir(), "lemon-headless-bundle-probe-");
const temporary = fs.mkdtempSync(prefix);
const bundlePath = path.join(temporary, "bundle.js");
const builderPath = path.join(development, "mobile/nodejs-client/build-node.py");
const buildCode = [
    "import importlib.util, sys",
    "spec = importlib.util.spec_from_file_location('review_builder', sys.argv[1])",
    "builder = importlib.util.module_from_spec(spec)",
    "spec.loader.exec_module(builder)",
    "builder.OUT = sys.argv[2]",
    "sys.exit(builder.main())"
].join("\n");

function noop()
{
}

try
{
    const built = spawnSync("python", ["-c", buildCode, builderPath, bundlePath], { encoding: "utf8" });
    assert.equal(built.status, 0, built.stderr || built.stdout);
    const source = fs.readFileSync(bundlePath, "utf8");
    assert.equal(source.includes("function server_info__clients_changed("), false);
    const errors = [];
    const processed = [];
    const pending = [];
    const context =
    {
        console:
        {
            log: noop,
            warn: noop,
            error(...messages)
            {
                errors.push(messages.map(String).join(" "));
            }
        },
        crypto: crypto.webcrypto,
        TextEncoder,
        TextDecoder,
        Buffer,
        performance,
        atob,
        btoa,
        setTimeout(callback)
        {
            pending.push(callback);
            return pending.length;
        },
        clearTimeout: noop,
        setInterval: noop,
        clearInterval: noop
    };
    context.global = context;
    vm.createContext(context);
    vm.runInContext(source, context, { filename: bundlePath, timeout: 10000 });
    assert.ok(context.what);
    context.what.init_node_runtime();
    context.what.set_on_message_processed(function(type, failed)
    {
        processed.push({ type, failed });
    });

    function drain()
    {
        let count = 0;
        while (pending.length > 0)
        {
            assert.ok(count++ < 30, "bounded pending callbacks");
            pending.shift()();
        }
    }

    drain();
    assert.deepEqual(errors, []);
    const cases = [
        ["client_list", { clients: [], local_username: "synthetic" }],
        ["client_connect", { client_id: 44, username: "synthetic" }],
        ["client_disconnect", { client_id: 44 }]
    ];
    for (const [kind, payload] of cases)
    {
        context.global.postMessage(
        {
            type: "data_processing_worker__" + kind + "_from_server",
            value: { message: { type: kind, ...payload } }
        });
        drain();
        assert.equal(processed.at(-1).failed, false, errors.join("\n"));
        console.log("PASS: fresh production bundle handles " + kind + " without optional server-info renderer.");
    }
    assert.equal(context.what.read_state().g_is_client_list_retrieved, true);

    // Control only: an optional presentation helper supplied to this VM allows the real list handler.
    // Production sources remain unchanged, and this is not a proposed blanket stub solution.
    context.server_info__clients_changed = noop;
    const errorCount = errors.length;
    context.global.postMessage(
    {
        type: "data_processing_worker__client_list_from_server",
        value: { message: { type: "client_list", clients: [], local_username: "synthetic" } }
    });
    drain();
    assert.equal(errors.length, errorCount, errors.slice(errorCount).join("\n"));
    assert.equal(processed.at(-1).failed, false);
    assert.equal(context.what.read_state().g_is_client_list_retrieved, true);
    console.log("CONTROL: supplying only the optional helper lets the real list handler set readiness.");

    delete context.server_info__clients_changed;
    context.global.postMessage({ type: "data_processing_worker__server_info", value: { message: {} } });
    drain();
    assert.equal(errors.length, errorCount);
    assert.equal(processed.at(-1).failed, false);
    console.log("CONTROL: already-guarded server-info dispatch tolerates the absent optional module.");
}
finally
{
    const resolved = path.resolve(temporary);
    assert.ok(resolved.startsWith(prefix), "cleanup must remain in the uniquely created tests directory");
    assert.equal(path.dirname(resolved), require("node:os").tmpdir());
    fs.rmSync(resolved, { recursive: true, force: true });
}
