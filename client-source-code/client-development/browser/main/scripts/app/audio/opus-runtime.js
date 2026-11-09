// The browser's Opus project exports plain C functions and owns its memory.
// No generated Emscripten runtime, filesystem, or network loader is needed.
var Module = {};
var asm = {};

function opus_runtime__refresh_heap()
{
    const buffer = Module.asm.memory.buffer;
    if (!Module.HEAPU8 || Module.HEAPU8.buffer !== buffer)
    {
        Module.HEAPU8 = new Uint8Array(buffer);
        Module.HEAP32 = new Int32Array(buffer);
        Module.HEAPF32 = new Float32Array(buffer);
    }
}

function opus_runtime__call(name, arguments_)
{
    try
    {
        return Module.asm[name](...arguments_);
    }
    finally
    {
        // malloc inside either the codec or resampler may grow WebAssembly memory.
        opus_runtime__refresh_heap();
    }
}

function create_opus_webassembly_instance()
{
    if (Module.asm)
    {
        return Module.asm;
    }
    const bytes = Uint8Array.from(atob('@@WASM:wasm/libopus.wasm@@'), character => character.charCodeAt(0));
    // Complete initialization before a worker can accept its first audio message.
    const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
    Module.asm = instance.exports;
    Module.asm._initialize();
    opus_runtime__refresh_heap();
    return Module.asm;
}

Module._malloc = function(size)
{
    const pointer = opus_runtime__call('malloc', [size]);
    if (!pointer)
    {
        throw new Error('Opus could not allocate ' + size + ' bytes');
    }
    return pointer;
};

Module._free = function(pointer)
{
    opus_runtime__call('free', [pointer]);
};

Module.getValue = function(pointer, type)
{
    if (type !== 'i32')
    {
        throw new Error('Unsupported Opus memory type: ' + type);
    }
    return Module.HEAP32[pointer >> 2];
};

Module.setValue = function(pointer, value, type)
{
    if (type !== 'i32')
    {
        throw new Error('Unsupported Opus memory type: ' + type);
    }
    Module.HEAP32[pointer >> 2] = value;
};

function _opus_encoder_create(...args)
{
    return opus_runtime__call('opus_encoder_create', args);
}

function _opus_encode_float(...args)
{
    return opus_runtime__call('opus_encode_float', args);
}

function _opus_encoder_ctl(...args)
{
    return opus_runtime__call('opus_encoder_ctl', args);
}

function _opus_encoder_destroy(...args)
{
    return opus_runtime__call('opus_encoder_destroy', args);
}

function _opus_decoder_create(...args)
{
    return opus_runtime__call('opus_decoder_create', args);
}

function _opus_decode_float(...args)
{
    return opus_runtime__call('opus_decode_float', args);
}

function _opus_decoder_ctl(...args)
{
    return opus_runtime__call('opus_decoder_ctl', args);
}

function _opus_decoder_destroy(...args)
{
    return opus_runtime__call('opus_decoder_destroy', args);
}

class SpeexResampler
{
    constructor(channels, inRate, outRate, quality = 5)
    {
        this.channels = channels;
        this.inRate = inRate;
        this.outRate = outRate;
        this.inPtr = 0;
        this.outPtr = 0;
        this.inCapacity = 0;
        this.outCapacity = 0;
        this.handle = this.inLenPtr = this.outLenPtr = 0;
        const error = Module._malloc(4);
        try
        {
            this.handle = opus_runtime__call('speex_resampler_init', [channels, inRate, outRate, quality, error]);
            if (!this.handle || Module.getValue(error, 'i32') !== 0)
            {
                throw new Error('Speex resampler initialization failed: ' + Module.getValue(error, 'i32'));
            }
            this.inLenPtr = Module._malloc(4);
            this.outLenPtr = Module._malloc(4);
        }
        catch (failure)
        {
            this.destroy();
            throw failure;
        }
        finally
        {
            Module._free(error);
        }
    }

    process(input)
    {
        if (!this.handle)
        {
            throw new Error('Speex resampler has been destroyed');
        }
        // Preserve input before an allocation can detach a view into the wasm heap.
        if (input.buffer === Module.HEAPF32.buffer)
        {
            input = input.slice();
        }
        const frames = input.length / this.channels;
        const outputFrames = Math.ceil(frames * this.outRate / this.inRate);
        const inputBytes = input.byteLength;
        const outputBytes = outputFrames * this.channels * 4;
        if (inputBytes > this.inCapacity)
        {
            const replacement = Module._malloc(inputBytes);
            Module._free(this.inPtr);
            this.inPtr = replacement;
            this.inCapacity = inputBytes;
        }
        if (outputBytes > this.outCapacity)
        {
            const replacement = Module._malloc(outputBytes);
            Module._free(this.outPtr);
            this.outPtr = replacement;
            this.outCapacity = outputBytes;
        }
        Module.HEAPF32.set(input, this.inPtr >> 2);
        Module.setValue(this.inLenPtr, frames, 'i32');
        Module.setValue(this.outLenPtr, outputFrames, 'i32');
        const result = opus_runtime__call('speex_resampler_process_interleaved_float',
            [this.handle, this.inPtr, this.inLenPtr, this.outPtr, this.outLenPtr]);
        if (result !== 0)
        {
            throw new Error('Speex resampling failed: ' + result);
        }
        const samples = Module.getValue(this.outLenPtr, 'i32') * this.channels;
        return Module.HEAPF32.subarray(this.outPtr >> 2, (this.outPtr >> 2) + samples);
    }

    destroy()
    {
        if (this.handle)
        {
            opus_runtime__call('speex_resampler_destroy', [this.handle]);
        }
        Module._free(this.inLenPtr);
        Module._free(this.outLenPtr);
        Module._free(this.inPtr);
        Module._free(this.outPtr);
        this.handle = this.inLenPtr = this.outLenPtr = this.inPtr = this.outPtr = 0;
    }
}
