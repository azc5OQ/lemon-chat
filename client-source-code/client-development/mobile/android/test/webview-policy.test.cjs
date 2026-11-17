"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const template = fs.readFileSync(path.resolve(__dirname, "../../../browser/main/template.html"), "utf8");
const policy = template.match(/<meta\s+http-equiv="Content-Security-Policy"\s+content="([^"]+)"\s*\/?\s*>/i);

test("bundled page restricts frames and objects before any script runs", function()
{
    assert.ok(policy, "the source template must carry the policy into every generated page");
    assert.ok(template.indexOf(policy[0]) < template.indexOf("<script"));
    assert.match(policy[1], /(?:^|;)\s*frame-src\s+'none'\s*(?:;|$)/);
    assert.match(policy[1], /(?:^|;)\s*object-src\s+'none'\s*(?:;|$)/);
});

test("browser blocks embedded documents while retaining inline code, Blob workers and Wasm", async function(context)
{
    if (Number(process.versions.node.split(".")[0]) < 20)
    {
        context.skip("Playwright tooling requires Node 20 or newer");
        return;
    }
    let chromium;
    try
    {
        chromium = require(process.env.LEMON_PLAYWRIGHT_PATH
            || "../../../../../tests/.browser-deps/node_modules/playwright-core").chromium;
    }
    catch (error)
    {
        context.skip("Install playwright-core or set LEMON_PLAYWRIGHT_PATH");
        return;
    }
    const executable = process.env.LEMON_BROWSER_PATH
        || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
    if (!fs.existsSync(executable))
    {
        context.skip("Set LEMON_BROWSER_PATH to an installed Chromium browser");
        return;
    }
    const browser = await chromium.launch({executablePath: executable, headless: true});
    try
    {
        const page = await browser.newPage();
        await page.setContent("<!doctype html><html><head>" + policy[0]
            + "<script>window.inline_code_ran = true;</script></head><body></body></html>");
        await page.evaluate(function()
        {
            window.violations = [];
            document.addEventListener("securitypolicyviolation", function(event)
            {
                window.violations.push(event.effectiveDirective);
            });
            const frame = document.createElement("iframe");
            frame.src = "data:text/html,blocked-frame";
            document.body.appendChild(frame);
            const object = document.createElement("object");
            object.type = "text/html";
            object.data = "data:text/html,blocked-object";
            document.body.appendChild(object);
        });
        await page.waitForFunction(function()
        {
            return window.violations.includes("frame-src") && window.violations.includes("object-src");
        });
        const result = await page.evaluate(async function()
        {
            const url = URL.createObjectURL(new Blob(["postMessage('worker-ready');"], {type: "text/javascript"}));
            let worker;
            try
            {
                const worker_result = await new Promise(function(resolve, reject)
                {
                    worker = new Worker(url);
                    worker.onmessage = function(event)
                    {
                        resolve(event.data);
                    };
                    worker.onerror = function()
                    {
                        reject(new Error("Blob worker failed under the bundled policy"));
                    };
                });
                const module = await WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
                return {inline: window.inline_code_ran, worker: worker_result,
                    wasm: module instanceof WebAssembly.Module, violations: window.violations};
            }
            finally
            {
                if (worker != null)
                {
                    worker.terminate();
                }
                URL.revokeObjectURL(url);
            }
        });
        assert.equal(result.inline, true);
        assert.equal(result.worker, "worker-ready");
        assert.equal(result.wasm, true);
        assert.deepEqual(new Set(result.violations), new Set(["frame-src", "object-src"]));
    }
    finally
    {
        await browser.close();
    }
});
