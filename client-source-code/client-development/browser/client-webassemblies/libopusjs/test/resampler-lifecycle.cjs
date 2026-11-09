const assert = require('node:assert/strict');
const vm = require('node:vm');

// Native handles need separate tracking: native Speex allocations bypass Module._malloc.
function checkResampler(context, legacy = false)
{
    const Resampler = legacy ? context.SpeexResampler : vm.runInContext('SpeexResampler', context);
    const module = context.Module;
    const malloc = module._malloc;
    const free = module._free;
    const runtimeCall = context.opus_runtime__call;
    const init = context._speex_resampler_init;
    const destroy = context._speex_resampler_destroy;
    const process = context._speex_resampler_process_interleaved_float;
    const buffers = new Map();
    const handles = new Set();
    let failAt = 0;
    let allocationCalls = 0;
    let processCalls = 0;

    module._malloc = function(size)
    {
        allocationCalls++;
        if (failAt !== 0 && allocationCalls === failAt)
        {
            if (legacy)
            {
                return 0;
            }
            throw new Error('Injected resampler allocation failure');
        }
        const pointer = malloc(size);
        if (pointer !== 0)
        {
            assert.ok(!buffers.has(pointer), 'allocator returned a still-owned buffer');
            buffers.set(pointer, size);
        }
        return pointer;
    };
    module._free = function(pointer)
    {
        if (pointer !== 0)
        {
            assert.ok(buffers.delete(pointer), 'buffer freed twice or without ownership');
        }
        return free(pointer);
    };

    function observe(name, args, invoke)
    {
        if (name === 'speex_resampler_destroy')
        {
            assert.ok(handles.delete(args[0]), 'native resampler destroyed twice or without ownership');
        }
        if (name === 'speex_resampler_process_interleaved_float')
        {
            assert.ok(handles.has(args[0]), 'processing needs a live native resampler');
            assert.ok(args[1] !== 0 && buffers.has(args[1]), 'processing needs an owned input buffer');
            assert.ok(args[3] !== 0 && buffers.has(args[3]), 'processing needs an owned output buffer');
            processCalls++;
        }
        const result = invoke();
        if (name === 'speex_resampler_init' && result !== 0)
        {
            assert.ok(!handles.has(result), 'native resampler handle reused while live');
            handles.add(result);
        }
        return result;
    }

    if (legacy)
    {
        context._speex_resampler_init = function(...args)
        {
            return observe('speex_resampler_init', args, () => init(...args));
        };
        context._speex_resampler_destroy = function(...args)
        {
            return observe('speex_resampler_destroy', args, () => destroy(...args));
        };
        context._speex_resampler_process_interleaved_float = function(...args)
        {
            return observe('speex_resampler_process_interleaved_float', args, () => process(...args));
        };
    }
    else
    {
        context.opus_runtime__call = function(name, args)
        {
            return observe(name, args, () => runtimeCall(name, args));
        };
    }

    function empty(label)
    {
        assert.equal(buffers.size, 0, label + ': JavaScript buffer leak');
        assert.equal(handles.size, 0, label + ': native resampler handle leak');
    }

    try
    {
        for (let failure = 1; failure <= 3; failure++)
        {
            allocationCalls = 0;
            failAt = failure;
            assert.throws(() => new Resampler(2, 44100, 48000), /allocation/i);
            failAt = 0;
            empty('constructor allocation failure ' + failure);
        }
        assert.throws(() => new Resampler(0, 44100, 48000), /init/i);
        empty('native initialization failure');

        for (const channels of [1, 2])
        {
            for (const failure of [1, 2])
            {
                const resampler = new Resampler(channels, 44100, 48000);
                try
                {
                    const small = new Float32Array(4410 * channels).fill(0.2);
                    const large = new Float32Array(8820 * channels).fill(0.3);
                    assert.equal(resampler.process(small).length, 4800 * channels);
                    assert.equal(handles.size, 1);
                    const oldInput = resampler.inPtr;
                    const oldOutput = resampler.outPtr;
                    const oldInputCapacity = resampler.inCapacity;
                    const oldOutputCapacity = resampler.outCapacity;
                    const processed = processCalls;
                    allocationCalls = 0;
                    failAt = failure;
                    assert.throws(() => resampler.process(large), /allocation/i);
                    failAt = 0;
                    assert.equal(processCalls, processed, 'failed allocation must not enter native processing');
                    if (failure === 1)
                    {
                        assert.equal(resampler.inPtr, oldInput);
                        assert.equal(resampler.inCapacity, oldInputCapacity);
                    }
                    assert.equal(resampler.outPtr, oldOutput);
                    assert.equal(resampler.outCapacity, oldOutputCapacity);
                    const retry = resampler.process(small);
                    assert.equal(retry.length, 4800 * channels);
                    assert.ok(retry.every(Number.isFinite));
                    assert.ok(retry.some(value => Math.abs(value) > 0.01));
                    const grown = resampler.process(large);
                    assert.equal(grown.length, 9600 * channels);
                    assert.ok(grown.every(Number.isFinite));
                }
                finally
                {
                    failAt = 0;
                    resampler.destroy();
                    resampler.destroy();
                }
                empty(channels + '-channel growth allocation failure ' + failure);
            }
        }
    }
    finally
    {
        module._malloc = malloc;
        module._free = free;
        if (legacy)
        {
            context._speex_resampler_init = init;
            context._speex_resampler_destroy = destroy;
            context._speex_resampler_process_interleaved_float = process;
        }
        else
        {
            context.opus_runtime__call = runtimeCall;
        }
    }
    console.log('PASS: ' + (legacy ? 'prebuilt' : 'current')
        + ' Speex native handles, constructor failures, input/output growth failure retries and double destroy.');
}

module.exports = {checkResampler};
