// the browser's Opus project exports plain C functions and owns its memory
// no generated Emscripten runtime, filesystem, or network loader is needed
var Module = {};
var asm = {};

/**
 * @brief recreates typed memory views when the Opus WebAssembly memory grows
 *
 * @return void
 */
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

/**
 * @brief calls an Opus export and refreshes memory views even when it throws
 *
 * @param string name -> exported function name
 * @param array arguments_ -> arguments passed to the export
 *
 * @return * -> the export's return value
 */
function opus_runtime__call(name, arguments_)
{
    try
    {
        return Module.asm[name](...arguments_);
    }
    finally
    {
        // malloc inside either the codec or resampler may grow WebAssembly memory
        opus_runtime__refresh_heap();
    }
}

/**
 * @brief initializes the embedded Opus module once before workers accept audio
 *
 * @return object -> initialized WebAssembly exports
 */
function create_opus_webassembly_instance()
{
    if (Module.asm)
    {
        return Module.asm;
    }
    const bytes = Uint8Array.from(atob('@@WASM:wasm/libopus.wasm@@'), character => character.charCodeAt(0));
    // complete initialization before a worker can accept its first audio message
    const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
    Module.asm = instance.exports;
    Module.asm._initialize();
    opus_runtime__refresh_heap();
    return Module.asm;
}

/**
 * @brief allocates bytes in Opus memory and refreshes its typed views
 *
 * @param number size -> number of bytes to allocate
 *
 * @return number -> allocated memory address
 *
 * @throws Error -> allocation returned a null pointer
 */
Module._malloc = function(size)
{
    const pointer = opus_runtime__call('malloc', [size]);
    if (!pointer)
    {
        throw new Error('Opus could not allocate ' + size + ' bytes');
    }
    return pointer;
};

/**
 * @brief releases an allocation in Opus memory
 *
 * @param number pointer -> memory address to free
 *
 * @return void
 */
Module._free = function(pointer)
{
    opus_runtime__call('free', [pointer]);
};

/**
 * @brief reads a signed 32 bit integer from Opus memory
 *
 * @param number pointer -> byte address to read
 * @param string type -> supported memory type, i32
 *
 * @return number -> stored integer
 *
 * @throws Error -> the requested type is not i32
 */
Module.getValue = function(pointer, type)
{
    if (type !== 'i32')
    {
        throw new Error('Unsupported Opus memory type: ' + type);
    }
    return Module.HEAP32[pointer >> 2];
};

/**
 * @brief writes a signed 32 bit integer into Opus memory
 *
 * @param number pointer -> byte address to write
 * @param number value -> integer to store
 * @param string type -> supported memory type, i32
 *
 * @return void
 *
 * @throws Error -> the requested type is not i32
 */
Module.setValue = function(pointer, value, type)
{
    if (type !== 'i32')
    {
        throw new Error('Unsupported Opus memory type: ' + type);
    }
    Module.HEAP32[pointer >> 2] = value;
};

/**
 * @brief creates an Opus encoder through the WebAssembly export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return number -> encoder pointer or zero on failure
 */
function _opus_encoder_create(...args)
{
    return opus_runtime__call('opus_encoder_create', args);
}

/**
 * @brief encodes floating point samples through the Opus export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return number -> encoded byte count or an Opus error code
 */
function _opus_encode_float(...args)
{
    return opus_runtime__call('opus_encode_float', args);
}

/**
 * @brief applies an encoder control request through the Opus export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return number -> Opus control result
 */
function _opus_encoder_ctl(...args)
{
    return opus_runtime__call('opus_encoder_ctl', args);
}

/**
 * @brief destroys an Opus encoder through the WebAssembly export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return void
 */
function _opus_encoder_destroy(...args)
{
    return opus_runtime__call('opus_encoder_destroy', args);
}

/**
 * @brief creates an Opus decoder through the WebAssembly export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return number -> decoder pointer or zero on failure
 */
function _opus_decoder_create(...args)
{
    return opus_runtime__call('opus_decoder_create', args);
}

/**
 * @brief decodes an Opus packet through the WebAssembly export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return number -> samples decoded per channel or an Opus error code
 */
function _opus_decode_float(...args)
{
    return opus_runtime__call('opus_decode_float', args);
}

/**
 * @brief applies a decoder control request through the Opus export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return number -> Opus control result
 */
function _opus_decoder_ctl(...args)
{
    return opus_runtime__call('opus_decoder_ctl', args);
}

/**
 * @brief destroys an Opus decoder through the WebAssembly export
 *
 * @param array args -> arguments for the matching Opus C export
 *
 * @return void
 */
function _opus_decoder_destroy(...args)
{
    return opus_runtime__call('opus_decoder_destroy', args);
}

class SpeexResampler
{
    /**
     * @brief creates a Speex resampler and allocates its input and output length fields
     *
     * @param number channels -> number of interleaved audio channels
     * @param number inRate -> input sample rate
     * @param number outRate -> output sample rate
     * @param number quality -> resampling quality, defaulting to 5
     *
     * @throws Error -> initialization or memory allocation fails
     */
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

    /**
     * @brief resamples interleaved PCM into a reusable WebAssembly output buffer
     *
     * @param Float32Array input -> interleaved input samples
     *
     * @return Float32Array -> view into reusable WebAssembly memory that later codec calls may replace or overwrite
     *
     * @throws Error -> the resampler is destroyed, allocation fails or resampling fails
     */
    process(input)
    {
        if (!this.handle)
        {
            throw new Error('Speex resampler has been destroyed');
        }
        // preserve input before an allocation can detach a view into the wasm heap
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
        const result = opus_runtime__call('speex_resampler_process_interleaved_float', [this.handle, this.inPtr, this.inLenPtr, this.outPtr, this.outLenPtr]);
        if (result !== 0)
        {
            throw new Error('Speex resampling failed: ' + result);
        }
        const samples = Module.getValue(this.outLenPtr, 'i32') * this.channels;
        return Module.HEAPF32.subarray(this.outPtr >> 2, (this.outPtr >> 2) + samples);
    }

    /**
     * @brief destroys the resampler and releases its allocated buffers
     *
     * @return void
     */
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
