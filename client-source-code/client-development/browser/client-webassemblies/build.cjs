const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');
const projects =
[
    ['libopusjs', 'libopus.wasm', 'test/codec.cjs', 'Opus audio codec and resampler'],
    ['mp3-to-pcm', 'mp3_decoder.wasm', 'test/decode.cjs', 'MP3 audio decoder'],
    ['rsa-keygen', 'rsa_keygen.wasm', null, 'RSA identity keys and operations'],
    ['connection-crypto', 'connection_crypto.wasm', 'test/ciphers.cjs', 'Connection ciphers and key derivation']
];

function run(script, arguments_ = [], env = process.env)
{
    const result = spawnSync(process.execPath, [script, ...arguments_], {stdio: 'inherit', env, windowsHide: true});
    if (result.error || result.status !== 0)
    {
        throw result.error || new Error(path.basename(script) + ' failed (' + result.status + ')');
    }
}

/**
 * @brief Tells whether the build that created a lock file is still running.
 *
 * @param string lockFile -> path of the lock file; it holds the process id of the build that created it
 *
 * @return boolean -> true when that build runs; false when it was stopped and left the file behind
 */
function isLockOwnerRunning(lockFile)
{
    let owner = 0;
    let age = 0;

    // read the process id
    try
    {
        owner = parseInt(fs.readFileSync(lockFile, 'utf8'), 10);
        age = Date.now() - fs.statSync(lockFile).mtimeMs;
    }
    catch (error)
    {
        return false;
    }

    // no id written
    if (!Number.isInteger(owner) || owner <= 0)
    {
        // a build writes its id right after it creates the file, so an empty file of the last seconds is in use
        return age < 10000;
    }

    // ask the system
    try
    {
        process.kill(owner, 0);
        return true;
    }
    catch (error)
    {
        return error.code === 'EPERM';
    }
}

function build(compiler, isTested = false)
{
    const buildDirectory = path.join(__dirname, 'build');
    fs.mkdirSync(buildDirectory, {recursive: true});
    const lockFile = path.join(buildDirectory, 'build.lock');
    let lock;
    try
    {
        lock = fs.openSync(lockFile, 'wx');
    }
    catch (error)
    {
        if (error.code !== 'EEXIST')
        {
            throw error;
        }

        // another build runs
        if (isLockOwnerRunning(lockFile))
        {
            throw new Error('Another WebAssembly build is running (lock file ' + lockFile + ').');
        }

        // remove leftover files
        console.log('An earlier WebAssembly build was stopped before it finished; removing its lock and its unfinished files.');
        fs.rmSync(lockFile, {force: true});
        for (const name of fs.readdirSync(buildDirectory))
        {
            if (name.startsWith('candidate-'))
            {
                fs.rmSync(path.join(buildDirectory, name), {recursive: true, force: true});
            }
        }

        // take the lock
        lock = fs.openSync(lockFile, 'wx');
    }

    // store own process id
    fs.writeSync(lock, String(process.pid));
    let temporary;
    try
    {
        console.log('\nWebAssembly: building ' + projects.length + ' modules from source:');
        for (const [index, [project, binary, test, description]] of projects.entries())
        {
            console.log('  ' + (index + 1) + '. ' + binary + ' - ' + description);
        }
        console.log('First compile all modules, ' + (isTested ? 'then test them, ' : '') + 'then embed them in client.html.\n');
        temporary = fs.mkdtempSync(path.join(buildDirectory, 'candidate-'));
        const candidate = path.join(temporary, 'wasm');
        fs.mkdirSync(candidate);
        const env = {...process.env, ZIG: compiler};
        for (const [index, [project, binary]] of projects.entries())
        {
            const step = '[WASM build ' + (index + 1) + '/' + projects.length + '] ';
            console.log('\n' + step + 'Starting ' + binary + ' (' + project + ')');
            run(path.join(__dirname, project, 'build.cjs'), [path.join(candidate, binary)], env);
            console.log(step + 'Finished ' + binary + '. Modules left to build: ' + (projects.length - index - 1) + '.');
        }
        if (!isTested)
        {
            console.log('\nAll ' + projects.length + ' modules compiled. The tests are skipped; --wasm-tests runs them.');
        }
        else
        {
            console.log('\nAll ' + projects.length + ' modules compiled. Starting tests.');
            for (const [index, [project, binary, test]] of projects.entries())
            {
                const step = '[WASM test ' + (index + 1) + '/' + projects.length + '] ';
                console.log('\n' + step + 'Testing ' + binary);
                if (test)
                {
                    run(path.join(__dirname, project, test), [path.join(candidate, binary)]);
                }
                else
                {
                    const rsaEnv = {...process.env, LEMON_RSA_WASM: path.join(candidate, binary)};
                    for (const [index, bits] of [512, 1024, 2048].entries())
                    {
                        console.log('  [RSA ' + (index + 1) + '/5] Checking ' + bits + '-bit identity vectors');
                        run(path.join(__dirname, 'rsa-keygen/test/compare-wasm.js'),
                            [path.join(__dirname, 'rsa-keygen/test/vectors-' + bits + '.json')], rsaEnv);
                    }
                    console.log('  [RSA 4/5] Checking modular arithmetic; this can take a while');
                    run(path.join(__dirname, 'rsa-keygen/test/rsa-ops.js'), [], rsaEnv);
                    console.log('  [RSA 5/5] Checking compatibility with the previous client');
                    run(path.join(__dirname, 'rsa-keygen/test/crypto-compat.js'), [], rsaEnv);
                }
                console.log(step + 'Passed ' + binary + '. Modules left to test: ' + (projects.length - index - 1) + '.');
            }
        }

        // Publish a complete set. A compiler/test failure leaves main/wasm untouched.
        console.log('\nPublishing the ' + projects.length + (isTested ? ' tested' : '') + ' WebAssembly modules.');
        const destination = path.resolve(__dirname, '../main/wasm');
        const previous = path.join(temporary, 'previous');
        if (fs.existsSync(destination))
        {
            fs.renameSync(destination, previous);
        }
        try
        {
            fs.renameSync(candidate, destination);
        }
        catch (error)
        {
            if (fs.existsSync(previous))
            {
                fs.renameSync(previous, destination);
            }
            throw error;
        }
        console.log('All four WebAssembly modules compiled' + (isTested ? ', tested' : '') + ' and published.');
        return destination;
    }
    finally
    {
        try
        {
            if (temporary)
            {
                fs.rmSync(temporary, {recursive: true, force: true});
            }
        }
        finally
        {
            fs.closeSync(lock);
            fs.rmSync(lockFile, {force: true});
        }
    }
}

module.exports = {build};
