"use strict";

const fs = require("node:fs");
const path = require("node:path");
const {spawnSync} = require("node:child_process");
const tests = fs.readdirSync(__dirname).filter(function(name)
{
    return name.endsWith(".test.cjs");
}).sort().map(function(name)
{
    return path.join(__dirname, name);
});
const result = spawnSync(process.execPath, ["--test", ...tests], {stdio: "inherit"});
if (result.error)
{
    console.error(result.error.message);
}
process.exit(result.status == null ? 1 : result.status);
