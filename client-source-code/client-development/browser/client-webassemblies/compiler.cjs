// Shared build plumbing only. Each WebAssembly project owns its C sources and libraries.
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

function compile(directory, name, arguments_)
{
    const output = path.resolve(process.argv[2] || path.join(directory, 'build', name));
    fs.mkdirSync(path.dirname(output), {recursive: true});
    const compiler = process.env.ZIG || 'zig';
    const args = ['-O2', '-g0', '-fno-ident', ...arguments_, '-Wl,--strip-all', '-o', output];
    // Response files avoid Windows' command line length limit for the Opus sources.
    const response = output + '.args';
    fs.writeFileSync(response, args.map(argument => JSON.stringify(argument.replaceAll('\\', '/'))).join('\n'));
    try
    {
        const result = spawnSync(compiler, ['cc', '@' + response], {stdio: 'inherit', windowsHide: true});
        if (result.error || result.status !== 0)
        {
            throw result.error || new Error(name + ': compiler exited with ' + result.status);
        }
        const moduleObject = new WebAssembly.Module(fs.readFileSync(output));
        const imports = WebAssembly.Module.imports(moduleObject);
        if (imports.length !== 0)
        {
            throw new Error(name + ' must not require host imports: ' + JSON.stringify(imports));
        }
        console.log('Built ' + name + ' (' + fs.statSync(output).size + ' bytes)');
    }
    finally
    {
        fs.rmSync(response, {force: true});
    }
}

module.exports = {compile};
