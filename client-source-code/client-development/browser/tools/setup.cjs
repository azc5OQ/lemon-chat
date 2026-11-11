const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const readline = require('node:readline/promises');
const {spawnSync} = require('node:child_process');
const versions = require('./versions.json');
const directory = path.resolve(__dirname, '../.build-tools');

async function allowDownload(description, policy)
{
    if (policy === 'yes')
    {
        return true;
    }
    if (policy === 'no' || !process.stdin.isTTY)
    {
        return false;
    }
    const prompt = readline.createInterface({input: process.stdin, output: process.stdout});
    try
    {
        const answer = await prompt.question(description + ' Download into browser/.build-tools? [y/N]: ');
        return /^y(es)?$/i.test(answer.trim());
    }
    finally
    {
        prompt.close();
    }
}

function execute(command, args)
{
    const result = spawnSync(command, args, {stdio: 'inherit', windowsHide: true});
    if (result.error || result.status !== 0)
    {
        throw result.error || new Error(command + ' failed (' + result.status + ')');
    }
}

async function ensureZig(policy)
{
    const package_ = versions.zig.packages[process.platform + '-' + process.arch];
    const cached = package_ ? path.join(directory, package_.directory, process.platform === 'win32' ? 'zig.exe' : 'zig') : null;
    const candidates = process.env.ZIG ? [process.env.ZIG] : ['zig', cached].filter(Boolean);
    let diagnostic = '';
    for (const candidate of candidates)
    {
        const result = spawnSync(candidate, ['version'], {encoding: 'utf8', windowsHide: true, timeout: 10000});
        if (result.status === 0 && result.stdout.trim() === versions.zig.version)
        {
            console.log('Using existing Zig ' + versions.zig.version + ': ' + candidate);
            return candidate;
        }
        diagnostic = result.error?.message || result.stderr?.trim() || ('reported version ' + result.stdout?.trim());
    }
    if (process.env.ZIG)
    {
        throw new Error('ZIG must point to the Zig ' + versions.zig.version + ' executable. Current value: '
            + process.env.ZIG + ' (' + diagnostic + ')');
    }
    if (!package_ || !await allowDownload('Zig ' + versions.zig.version + ' is needed to compile the four WebAssembly modules.', policy))
    {
        throw new Error('Install Zig ' + versions.zig.version + ' from https://ziglang.org/download/ and set ZIG to its executable, '
            + 'or use --download-tools. The existing client.html was kept.');
    }
    fs.mkdirSync(directory, {recursive: true});
    const archive = path.join(directory, path.basename(package_.url));
    console.log('Downloading ' + package_.url);
    execute(process.platform === 'win32' ? 'curl.exe' : 'curl',
        ['--fail', '--location', '--retry', '2', '--output', archive, package_.url]);
    const hash = crypto.createHash('sha256').update(fs.readFileSync(archive)).digest('hex');
    if (hash !== package_.sha256)
    {
        fs.rmSync(archive, {force: true});
        throw new Error('Zig download checksum mismatch; nothing was installed.');
    }
    execute('tar', ['-xf', archive, '-C', directory]);
    const result = spawnSync(cached, ['version'], {encoding: 'utf8', windowsHide: true});
    if (result.status !== 0 || result.stdout.trim() !== versions.zig.version)
    {
        throw new Error('Downloaded Zig could not run: ' + cached);
    }
    fs.rmSync(archive, {force: true});
    console.log('Using downloaded Zig ' + versions.zig.version + ': ' + cached);
    return cached;
}

async function installMinifier(policy)
{
    if (!await allowDownload('Terser is optional; it makes the final HTML smaller.', policy))
    {
        return false;
    }
    // npm's JS entry avoids shell quoting and also works with the portable Node download.
    const candidates = [path.join(path.dirname(process.execPath), 'node_modules/npm/bin/npm-cli.js'),
        path.resolve(path.dirname(process.execPath), '../lib/node_modules/npm/bin/npm-cli.js'),
        '/usr/share/nodejs/npm/bin/npm-cli.js'];
    const npm = candidates.find(file => fs.existsSync(file));
    if (!npm)
    {
        throw new Error('npm was not found beside Node.js. Install Terser yourself or choose --unminified.');
    }
    execute(process.execPath, [npm, 'install', '--prefix', path.join(directory, 'terser'),
        '--ignore-scripts', '--no-audit', '--no-fund', '--save-exact', 'terser@' + versions.terser]);
    return true;
}

module.exports = {ensureZig, installMinifier};
