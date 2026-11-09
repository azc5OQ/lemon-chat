#!/usr/bin/env node
// Compile and test the WebAssembly projects, then assemble one self-contained HTML file.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const zlib = require('node:zlib');
const crypto = require('node:crypto');
const {spawnSync} = require('node:child_process');
const readline = require('node:readline/promises');

const sourceDirectory = path.join(__dirname, 'main');
const defaultOutput = path.resolve(__dirname, '../../client.html');
const threadMarker = 'var THREAD_NAME = ';
const exportSeam = '// the export seam for the headless node build:';
const installHint = 'Use build.bat / sh build.sh to offer tool downloads, or install Node.js, Zig 0.15.2 and optional Terser yourself.';
const cssTokens = /(?<comment>\/\*[\s\S]*?\*\/)|(?<url>url\(\s*(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|(?:\\.|[^)\\])*)\s*\))|(?<string>"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(?<escape>\\[\s\S])|(?<space>\s+)|(?<plain>[^\s])/gi;

function rawElements(html, tag)
{
    const elements = [];
    const pattern = /<!--[\s\S]*?-->|(<(script|style)\b(?:[^"'<>]|"[^"]*"|'[^']*')*>)([\s\S]*?)(<\/\2\s*>)/gi;
    for (const match of html.matchAll(pattern))
    {
        if (match[2]?.toLowerCase() === tag)
        {
            elements.push({opening: match.index, start: match.index + match[1].length,
                end: match.index + match[1].length + match[3].length,
                closing: match.index + match[0].length, header: match[1], body: match[3]});
        }
    }
    return elements;
}

function assemble(wasmDirectory = path.join(sourceDirectory, 'wasm'), prebuilt = false)
{
    let includes = 0;
    let assemblies = 0;
    let html = fs.readFileSync(path.join(sourceDirectory, 'template.html'), 'utf8').split('\n').map(line =>
    {
        const match = /^\s*\/\*\s*@@INCLUDE:\s*(.+?)\s*@@\s*\*\/\s*$/.exec(line);
        if (!match)
        {
            return line;
        }
        includes++;
        const included = match[1].trim();
        const file = prebuilt && included === 'scripts/app/audio/opus-runtime.js'
            ? path.join(wasmDirectory, 'libopus.emscripten.js') : path.join(sourceDirectory, included);
        return fs.readFileSync(file, 'utf8');
    }).join('\n');
    html = html.replace(/@@WASM:([^@]+)@@/g, (_match, file) =>
    {
        assemblies++;
        return fs.readFileSync(path.join(wasmDirectory, path.basename(file.trim()))).toString('base64');
    });
    return {html, includes, assemblies};
}

function checkScript(source)
{
    new vm.Script(source, {filename: 'assembled-browser-script.js'});
    if (source.includes(threadMarker))
    {
        const declaration = /var THREAD_NAME = "( +)"/.exec(source);
        if (!declaration || declaration[1].length < 'data_processing_worker'.length
            || source.split(threadMarker).length !== 3 || source.indexOf(threadMarker) >= declaration.index)
        {
            throw new Error('Worker THREAD_NAME patch target or padding changed.');
        }
    }
}

function checkHtml(html)
{
    const scripts = rawElements(html, 'script');
    if (!scripts.length)
    {
        throw new Error('No script elements found in the client.');
    }
    for (const script of scripts)
    {
        checkScript(script.body);
    }
}

// Prefer the locally downloaded minifier, then existing installations.
function findMinifier()
{
    const candidates = [path.join(__dirname, '.build-tools/terser/node_modules/terser'), 'terser'];
    const probe = process.platform === 'win32'
        ? spawnSync(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', 'npm root -g'],
            {encoding: 'utf8', windowsHide: true, timeout: 10000})
        : spawnSync('npm', ['root', '-g'], {encoding: 'utf8', timeout: 10000});
    if (probe.status === 0 && probe.stdout.trim())
    {
        candidates.push(path.join(probe.stdout.trim(), 'terser'));
    }
    for (const candidate of candidates)
    {
        try
        {
            const terser = require(candidate);
            const version = require(candidate === 'terser' ? 'terser/package.json' : path.join(candidate, 'package.json')).version;
            if (typeof terser.minify === 'function' && Number(version.split('.')[0]) >= 5)
            {
                return {minify: terser.minify, version};
            }
        }
        catch (error)
        {
            // Try the next installed location; an unavailable tool is shown as disabled.
        }
    }
    return null;
}

async function minifyScript(source, tool)
{
    const originalThread = source.match(/var THREAD_NAME = "( +)"/g) || [];
    const result = await tool.minify(source,
    {
        compress: false,
        mangle: false,
        keep_fnames: true,
        keep_classnames: true,
        format:
        {
            braces: true,
            keep_numbers: true,
            keep_quoted_props: true,
            quote_style: 3,
            inline_script: true,
            comments: (_node, comment) => /^!|copyright|licen[cs]e|@preserve|the export seam for the headless node build:/i.test(comment.value)
        }
    });
    let output = result.code;
    if (originalThread.length)
    {
        const compactThread = /var THREAD_NAME="( +)"/g;
        if (originalThread.length !== 1 || [...output.matchAll(compactThread)].length !== 1)
        {
            throw new Error('Minifier changed the worker patch declaration.');
        }
        output = output.replace(compactThread, originalThread[0]);
    }
    if (source.includes(exportSeam) && output.split(exportSeam).length !== 2)
    {
        throw new Error('Minifier removed the headless export seam.');
    }
    checkScript(output);
    return output;
}

function checksum(data)
{
    let sum = 0;
    for (let offset = 0; offset < data.length; offset += 4)
    {
        let word = 0;
        for (let index = 0; index < 4; index++)
        {
            word = ((word << 8) | (data[offset + index] || 0)) >>> 0;
        }
        sum = (sum + word) >>> 0;
    }
    return sum;
}

// Lossless WOFF packaging, with every input table and its checksums retained.
function fontToWoff(raw)
{
    if (raw.length < 12 || !['00010000', '4f54544f', '74727565'].includes(raw.subarray(0, 4).toString('hex')))
    {
        throw new Error('Unsupported font');
    }
    const count = raw.readUInt16BE(4);
    const exponent = Math.floor(Math.log2(count));
    if (!count || 12 + count * 16 > raw.length || raw.readUInt16BE(6) !== 2 ** exponent * 16
        || raw.readUInt16BE(8) !== exponent || raw.readUInt16BE(10) !== count * 16 - 2 ** exponent * 16)
    {
        throw new Error('Invalid font directory');
    }
    const tables = [];
    const tags = new Set();
    for (let index = 0; index < count; index++)
    {
        const entry = 12 + index * 16;
        const tag = raw.toString('ascii', entry, entry + 4);
        const expected = raw.readUInt32BE(entry + 4);
        const offset = raw.readUInt32BE(entry + 8);
        const length = raw.readUInt32BE(entry + 12);
        if (tags.has(tag) || offset % 4 || offset < 12 + count * 16 || offset + length > raw.length)
        {
            throw new Error('Invalid font table bounds');
        }
        tags.add(tag);
        const data = raw.subarray(offset, offset + length);
        const checked = Buffer.from(data);
        if (tag === 'head')
        {
            if (length < 54)
            {
                throw new Error('Truncated font head');
            }
            checked.fill(0, 8, 12);
        }
        if (checksum(checked) !== expected)
        {
            throw new Error('Invalid font table checksum');
        }
        let packed = zlib.deflateSync(data, {level: 9});
        if (packed.length >= data.length)
        {
            packed = data;
        }
        else if (!zlib.inflateSync(packed).equals(data))
        {
            throw new Error('Font compression did not round-trip');
        }
        tables.push({tag, expected, offset, data, packed});
    }
    if (!tags.has('head') || checksum(raw) !== 0xb1b0afba)
    {
        throw new Error('Invalid whole-font checksum');
    }
    tables.sort((a, b) => a.offset - b.offset);
    let originalOffset = 12 + count * 16;
    let offset = 44 + count * 20;
    const directory = [];
    const payload = [];
    const header = Buffer.alloc(44);
    header.write('wOFF');
    raw.copy(header, 4, 0, 4);
    header.writeUInt16BE(count, 12);
    header.writeUInt32BE(raw.length, 16);
    for (const table of tables)
    {
        const end = table.offset + table.data.length;
        const paddedEnd = Math.ceil(end / 4) * 4;
        if (table.offset !== originalOffset || raw.subarray(end, paddedEnd).some(byte => byte !== 0))
        {
            throw new Error('Noncanonical font padding');
        }
        originalOffset = paddedEnd;
        if (table.tag === 'head')
        {
            table.data.copy(header, 20, 4, 8);
        }
        const entry = Buffer.alloc(20);
        entry.write(table.tag);
        entry.writeUInt32BE(offset, 4);
        entry.writeUInt32BE(table.packed.length, 8);
        entry.writeUInt32BE(table.data.length, 12);
        entry.writeUInt32BE(table.expected, 16);
        directory.push(entry);
        const padded = Buffer.alloc(Math.ceil(table.packed.length / 4) * 4);
        table.packed.copy(padded);
        payload.push(padded);
        offset += padded.length;
    }
    if (originalOffset !== raw.length)
    {
        throw new Error('Font contains trailing data');
    }
    header.writeUInt32BE(offset, 8);
    directory.sort(Buffer.compare);
    return Buffer.concat([header, ...directory, ...payload]);
}

function urlValue(token)
{
    const value = token.slice(4, -1).trim();
    return ['"', "'"].includes(value[0]) ? value.slice(1, -1) : value;
}

function imageUrl(token)
{
    const value = urlValue(token);
    const match = /^data:([^;,]+);base64,([A-Za-z0-9+/]*={0,2})$/i.exec(value);
    if (match && (match[1].toLowerCase().startsWith('image/')
        || (match[1].toLowerCase() === 'application/octet-stream'
            && Buffer.from(match[2].slice(0, 16), 'base64').subarray(0, 8).equals(Buffer.from('89504e470d0a1a0a', 'hex')))))
    {
        return value;
    }
    return null;
}

function convertFonts(css)
{
    return css.replace(/(@font-face\s*\{)([^{}]*)(\})/gi, (_match, opening, body, closing) =>
    {
        let converted = false;
        let result = '';
        for (const token of body.matchAll(cssTokens))
        {
            let value = token[0];
            if (token.groups.url)
            {
                const font = /^data:(?:application\/octet-stream|font\/ttf|application\/x-font-ttf);base64,([A-Za-z0-9+/]*={0,2})$/i.exec(urlValue(value));
                if (font)
                {
                    const raw = Buffer.from(font[1], 'base64');
                    try
                    {
                        const packed = fontToWoff(raw);
                        if (packed.length < raw.length)
                        {
                            value = 'url("data:font/woff;base64,' + packed.toString('base64') + '")';
                            converted = true;
                        }
                    }
                    catch (error)
                    {
                        console.warn('Font kept unchanged: ' + error.message);
                    }
                }
            }
            result += value;
        }
        if (converted)
        {
            result = result.replace(/format\(\s*(['"])truetype\1\s*\)/gi, 'format("woff")');
        }
        return opening + result + closing;
    });
}

function compactCss(css, images = new Map())
{
    const output = [];
    let pendingSpace = false;
    for (const token of css.matchAll(cssTokens))
    {
        let value = token[0];
        if (token.groups.space)
        {
            pendingSpace = true;
            continue;
        }
        if (token.groups.comment && !/copyright|licen[cs]e|@preserve|^\/\*!/i.test(value))
        {
            const before = css[token.index - 1];
            const after = css[token.index + value.length];
            if (!before || !after || /\s/.test(before) || /\s/.test(after))
            {
                continue;
            }
            value = '/**/';
        }
        if (pendingSpace && output.length && !'{};'.includes(output.at(-1).slice(-1)) && !'{};'.includes(value[0]))
        {
            output.push(' ');
        }
        pendingSpace = false;
        const image = token.groups.url ? imageUrl(value) : null;
        if (images.has(image))
        {
            value = 'var(' + images.get(image) + ')';
        }
        output.push(value);
    }
    return output.join('');
}

function optimizeStyles(html)
{
    const elements = rawElements(html, 'style');
    const styles = elements.map(element => convertFonts(element.body));
    const counts = new Map();
    for (const css of styles)
    {
        for (const token of css.matchAll(cssTokens))
        {
            const image = token.groups.url ? imageUrl(token[0]) : null;
            if (image)
            {
                counts.set(image, (counts.get(image) || 0) + 1);
            }
        }
    }
    const images = new Map();
    for (const [image, count] of counts)
    {
        const name = '--lemon-build-image-' + crypto.createHash('sha256').update(image).digest('hex').slice(0, 16);
        if (count > 1 && count * (image.length + 7) > name.length + image.length + 9 + count * (name.length + 5))
        {
            if (html.includes(name))
            {
                throw new Error('Generated image variable conflicts with source: ' + name);
            }
            images.set(image, name);
        }
    }
    for (let index = elements.length - 1; index >= 0; index--)
    {
        const element = elements[index];
        html = html.slice(0, element.start) + compactCss(styles[index], images) + html.slice(element.end);
    }
    if (images.size)
    {
        const shared = '<style id="style-build-images">:root{' + [...images].map(([image, name]) =>
            name + ':url("' + image + '");').join('') + '}</style>\n';
        html = html.slice(0, elements[0].opening) + shared + html.slice(elements[0].opening);
    }
    return html;
}

function writeAtomic(file, content)
{
    fs.mkdirSync(path.dirname(file), {recursive: true});
    const temporary = path.join(path.dirname(file), '.lemon-client-' + crypto.randomUUID() + '.tmp');
    try
    {
        fs.writeFileSync(temporary, content, {encoding: 'utf8', flag: 'wx'});
        fs.renameSync(temporary, file);
    }
    finally
    {
        fs.rmSync(temporary, {force: true});
    }
}

function options(arguments_)
{
    const result = {mode: null, output: null, serverCopy: true, check: false, prebuilt: false, downloads: 'ask'};
    const modes = {'--minified': 'minified', '--minify-js': 'minified', '--unminified': 'unminified',
        '--no-optimize': 'unminified', '--no-minify-js': 'assets'};
    for (let index = 0; index < arguments_.length; index++)
    {
        const argument = arguments_[index];
        if (modes[argument])
        {
            if (result.mode)
            {
                throw new Error('Choose only one build mode.');
            }
            result.mode = modes[argument];
        }
        else if (argument === '--output' && arguments_[index + 1] && !arguments_[index + 1].startsWith('--'))
        {
            result.output = path.resolve(arguments_[++index]);
        }
        else if (argument === '--no-server-copy')
        {
            result.serverCopy = false;
        }
        else if (argument === '--prebuilt-wasm')
        {
            result.prebuilt = true;
        }
        else if (argument === '--download-tools' || argument === '--no-download-tools')
        {
            const policy = argument === '--download-tools' ? 'yes' : 'no';
            if (result.downloads !== 'ask' && result.downloads !== policy)
            {
                throw new Error('Choose only one tool-download policy.');
            }
            result.downloads = policy;
        }
        else if (argument === '--check')
        {
            result.check = true;
        }
        else
        {
            throw new Error('Unknown or incomplete option: ' + argument);
        }
    }
    if (!result.mode && !result.check)
    {
        result.mode = process.env.LEMON_CLIENT_BUILD_MODE?.trim().toLowerCase() || null;
        if (result.mode && !['minified', 'unminified'].includes(result.mode))
        {
            throw new Error('LEMON_CLIENT_BUILD_MODE must be minified or unminified.');
        }
    }
    return result;
}

async function chooseMode(tool)
{
    console.log('\n1. Minified' + (tool ? ' (Terser ' + tool.version + ')' : ' [disabled: Terser is not installed]'));
    console.log('2. Unminified (readable JavaScript and CSS)');
    if (!tool)
    {
        console.log(installHint);
    }
    if (!process.stdin.isTTY)
    {
        throw new Error('Use --unminified or --minified for an unattended build.');
    }
    const prompt = readline.createInterface({input: process.stdin, output: process.stdout});
    try
    {
        for (;;)
        {
            const answer = (await prompt.question(tool ? 'Choose [1/2]: ' : 'Choose [2]: ')).trim();
            if (answer === '2' || (answer === '1' && tool))
            {
                return answer === '1' ? 'minified' : 'unminified';
            }
        }
    }
    finally
    {
        prompt.close();
    }
}

async function main(arguments_ = process.argv.slice(2))
{
    if (arguments_.includes('--help'))
    {
        console.log('node build.cjs [--minified | --unminified] [--output file] [--no-server-copy]\n'
            + '  [--download-tools | --no-download-tools] [--prebuilt-wasm]\n'
            + 'Normal builds compile and test all four WebAssembly modules before embedding.\n'
            + '--prebuilt-wasm verifies and uses the checked prebuilt modules explicitly.\n'
            + 'node build.cjs --check (syntax only; no compilation or downloads)\nnode build.cjs --check-minifier\n' + installHint);
        return;
    }
    if (arguments_.includes('--check-minifier'))
    {
        const tool = findMinifier();
        if (!tool)
        {
            throw new Error('Minification disabled. ' + installHint);
        }
        await minifyScript('const toolCheck = 1;', tool);
        console.log('Terser ' + tool.version + ' is available.');
        return;
    }
    const settings = options(arguments_);
    let tool = !settings.check && (!settings.mode || settings.mode === 'minified') ? findMinifier() : null;
    const setup = require('./tools/setup.cjs');
    if (!settings.check)
    {
        if (!tool && (!settings.mode || settings.mode === 'minified') && await setup.installMinifier(settings.downloads))
        {
            tool = findMinifier();
        }
        settings.mode ||= await chooseMode(tool);
        if (settings.mode === 'minified' && !tool)
        {
            throw new Error('Minification disabled. ' + installHint);
        }
    }
    const webassemblies = require('./client-webassemblies/build.cjs');
    let wasmDirectory = path.join(sourceDirectory, 'wasm');
    if (settings.prebuilt)
    {
        wasmDirectory = webassemblies.backup();
    }
    else if (!settings.check)
    {
        wasmDirectory = webassemblies.build(await setup.ensureZig(settings.downloads));
    }
    if (!settings.check)
    {
        console.log('\nAssembling client.html and embedding the four WebAssembly modules as Base64.');
    }
    const assembled = assemble(wasmDirectory, settings.prebuilt);
    checkHtml(assembled.html);
    if (settings.check)
    {
        console.log('Client syntax and worker patch checks passed. No files written.');
        return;
    }
    if (settings.mode !== 'unminified')
    {
        console.log('Optimizing styles and embedded fonts.');
    }
    let html = settings.mode === 'unminified' ? assembled.html : optimizeStyles(assembled.html);
    if (settings.mode === 'minified')
    {
        console.log('Minifying JavaScript with Terser ' + tool.version + '.');
        for (const script of rawElements(html, 'script').reverse())
        {
            if (!script.body.trim() || /\bid\s*=\s*["']lemon-cfg["']|\bsrc\s*=/i.test(script.header))
            {
                continue;
            }
            html = html.slice(0, script.start) + '\n' + await minifyScript(script.body, tool) + '\n' + html.slice(script.end);
        }
        console.log('JavaScript: external Terser ' + tool.version + '; names and code retained.');
    }
    const output = settings.output || defaultOutput;
    writeAtomic(output, html);
    const serverDirectory = path.resolve(__dirname, '../../../buildresult');
    if (!settings.output && settings.serverCopy && fs.existsSync(serverDirectory))
    {
        writeAtomic(path.join(serverDirectory, 'client.html'), html);
        console.log('Updated ' + path.join(serverDirectory, 'client.html'));
    }
    console.log(settings.mode + ': ' + assembled.includes + ' includes + ' + assembled.assemblies + ' WebAssembly modules');
    console.log(Buffer.byteLength(assembled.html) + ' -> ' + Buffer.byteLength(html) + ' bytes; ' + output);
}

if (require.main === module)
{
    main().catch(error =>
    {
        console.error('Client build failed: ' + error.message);
        process.exitCode = 1;
    });
}

module.exports = {assemble, checkHtml, findMinifier, minifyScript, fontToWoff, compactCss, optimizeStyles, options};
