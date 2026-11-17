"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const root = path.resolve(__dirname, "../../../browser/main/scripts/app");

async function run()
{
    if (Number(process.versions.node.split(".")[0]) < 20)
    {
        test("browser FileReader regression", {skip: "Playwright requires Node 20 or newer; covered by the desktop Node suite"}, function()
        {
        });
        return;
    }
    let chromium;
    try
    {
        chromium = require(process.env.LEMON_PLAYWRIGHT_PATH || "../../../../../tests/.browser-deps/node_modules/playwright-core").chromium;
    }
    catch (error)
    {
        console.log("SKIP browser FileReader regression: install playwright-core or set LEMON_PLAYWRIGHT_PATH");
        return;
    }
    const executable = process.env.LEMON_BROWSER_PATH || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
    if (!fs.existsSync(executable))
    {
        console.log("SKIP browser FileReader regression: set LEMON_BROWSER_PATH to an installed Chromium browser");
        return;
    }
    const browser = await chromium.launch(
    {
        executablePath: executable,
        headless: true
    });
    try
    {
        const page = await browser.newPage();
        await page.setContent('<p id="identity-file-status"></p><button id="identity-file-confirm-button"></button>');
        await page.addScriptTag({content: "var G_ALLOWED_RSA_KEY_BITS=[2048,3072,4096,6144,8192];\n"
            + fs.readFileSync(path.join(root, "crypto/identity-file.js"), "utf8")
            + fs.readFileSync(path.join(root, "ui/ui.js"), "utf8")});
        const result = await page.evaluate(async function()
        {
            async function read(bytes)
            {
                UI.identity_read_identity_file(new File([bytes], "test.lmn"));
                for (let attempt = 0; attempt < 100; attempt++)
                {
                    if (!document.getElementById("identity-file-status").textContent.startsWith("Reading"))
                    {
                        break;
                    }
                    await new Promise(function(resolve)
                    {
                        setTimeout(resolve, 10);
                    });
                }
                return {
                    status: document.getElementById("identity-file-status").textContent,
                    disabled: document.getElementById("identity-file-confirm-button").disabled,
                    record: pending_identity_file
                };
            }
            const seed = " " + "A".repeat(200) + "\n";
            const good = await read(new TextEncoder().encode(identity_file__serialize(seed, 3072)));
            const badBytes = new TextEncoder().encode(identity_file__serialize(seed, 3072));
            badBytes[badBytes.indexOf(65)] = 255;
            const bad = await read(badBytes);
            const legacy = await read(new TextEncoder().encode(seed));
            UI.identity_read_identity_file(new File([identity_file__serialize(seed, 2048)], "stale.lmn"));
            UI.identity_clear_file_selection();
            await new Promise(function(resolve)
            {
                setTimeout(resolve, 100);
            });
            return {seed, good, bad, legacy, stalePending: pending_identity_file,
                staleDisabled: document.getElementById("identity-file-confirm-button").disabled};
        });
        assert.equal(result.good.record.passphrase, result.seed);
        assert.equal(result.good.record.rsa_key_bits, 3072);
        assert.equal(result.good.disabled, false);
        assert.equal(result.bad.record, null);
        assert.equal(result.bad.disabled, true);
        assert.match(result.bad.status, /invalid UTF-8/);
        assert.equal(result.legacy.record.passphrase, result.seed);
        assert.equal(result.legacy.record.legacy, true);
        assert.equal(result.stalePending, null);
        assert.equal(result.staleDisabled, true);
        console.log("PASS actual browser FileReader: strict bytes, exact JSON/raw seeds, invalid input cannot confirm, canceled read cannot restore selection");
    }
    finally
    {
        await browser.close();
    }
}

run().catch(function(error)
{
    console.error(error);
    process.exitCode = 1;
});
