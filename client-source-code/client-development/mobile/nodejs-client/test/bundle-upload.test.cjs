const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const { spawnSync } = require("node:child_process");

test("fresh Node bundle waits for remote socket capacity and cancels replaced upload", async function()
{
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), "lemon-bundle-upload-"));
    const output = path.join(directory, "bundle.js");
    try
    {
        const build = spawnSync("python", ["-c", [
            "import importlib.util,sys",
            "spec=importlib.util.spec_from_file_location('builder',sys.argv[1])",
            "builder=importlib.util.module_from_spec(spec)",
            "spec.loader.exec_module(builder)",
            "builder.OUT=sys.argv[2]",
            "sys.exit(builder.main())"
        ].join("\n"), path.resolve(__dirname, "../build-node.py"), output], { encoding: "utf8" });
        assert.equal(build.status, 0, build.stderr || build.stdout);
        let source = fs.readFileSync(output, "utf8");
        const anchor = "node_forward_upload_request: function(request)";
        assert.ok(source.includes(anchor));
        // Expose only fixture setup in the freshly generated closure. The real upload function,
        // fallback reply port, async Node worker and socket-capacity handler execute unchanged.
        source = source.replace(anchor, `transport_test_setup: function(socket)
        {
            g_is_authenticated = true;
            g_worker_connection_ended = false;
            g_worker_connection_generation = 7;
            websocket_connection_generation = 7;
            websocket_instance = socket;
            g_loopback_port = 0;
            keys__encrypt_all_message_data_and_convert_to_base64 = function(value)
            {
                return value;
            };
            return {
                flush: workers__flush_upload_send,
                replace: function()
                {
                    websocket_generation++;
                    g_worker_connection_generation++;
                }
            };
        },
        ` + anchor);
        const timers = new Map();
        const errors = [];
        let timer_id = 0;
        const context = {
            process: {}, crypto: crypto.webcrypto, Buffer, TextDecoder, TextEncoder, performance, atob, btoa,
            console: {
                log: function()
                {
                }, warn: function() {},
                error: function(value)
                {
                    errors.push(String(value));
                }
            },
            setTimeout: function(callback, delay)
            {
                const id = ++timer_id;
                timers.set(id, { callback, delay });
                return id;
            },
            clearTimeout: function(id)
            {
                timers.delete(id);
            },
            setInterval: function()
            {
            }, clearInterval: function() {}
        };
        context.global = context;
        vm.createContext(context);
        vm.runInContext(source, context, { timeout: 10000 });
        context.what.init_node_runtime();
        function drain_immediate()
        {
            let iterations = 0;
            while (true)
            {
                const next = Array.from(timers).find(function(entry)
                {
                    return !entry[1].delay;
                });
                if (!next)
                {
                    return;
                }
                assert.ok(iterations++ < 100);
                timers.delete(next[0]);
                next[1].callback();
            }
        }
        drain_immediate();
        const sent = [];
        const socket = {
            readyState: 1,
            bufferedAmount: 100000000,
            send: function(value)
            {
                sent.push(value);
            }
        };
        const fixture = context.what.transport_test_setup(socket);
        let settled = false;
        const upload = context.what.node_forward_upload_request({ message: { type: "file_send_chunk", chunk: "abc" } }).then(function(result)
        {
            settled = true;
            return result;
        });
        drain_immediate();
        await Promise.resolve();
        assert.equal(settled, false, "local worker dispatch is not remote capacity acknowledgement");
        assert.deepEqual(sent, []);
        socket.bufferedAmount = 0;
        fixture.flush();
        assert.equal(await upload, true);
        assert.equal(sent.length, 1);
        socket.bufferedAmount = 100000000;
        const replaced = context.what.node_forward_upload_request({ message: { type: "file_send_chunk", chunk: "def" } });
        drain_immediate();
        fixture.replace();
        socket.bufferedAmount = 0;
        fixture.flush();
        assert.equal(await replaced, false);
        assert.equal(sent.length, 1);
        assert.deepEqual(errors, []);
    }
    finally
    {
        fs.unlinkSync(output);
        fs.rmdirSync(directory);
    }
});
