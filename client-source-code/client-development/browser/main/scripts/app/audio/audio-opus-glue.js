// encodes microphone and stream audio into Opus packets and decodes received packets
// audio.js and dispatch.js send work to the codec workers. voice.js sends their output
// here a frame is one Opus packet. in audio.js it is one stereo sample pair

// state private to this file
var encoder = null;        // the microphone's encoder
var stream_encoder = null; // separate encoder for a video stream's sound. null when no stream audio is active


// Opus tuning constants. keep decoder output stereo, the sample rate at 48000 and the reset control at 4028
// playback buffering in audio.js also affects listener delay

// allocate one decoder per simultaneous speaker because Opus retains stream state
// released decoders return to the pool for reuse
var OPUS_DECODER_POOL_SIZE = 16;

// microphone frame length in milliseconds. Opus allows 2.5, 5, 10, 20, 40 or 60
// larger frames reduce packet overhead but add delay and loss per missing packet
// must not exceed OPUS_DECODER_FRAME_CAPACITY_MS
var OPUS_ENCODER_FRAME_DURATION_MS = 40;

// opus encoder mode: 2048 = OPUS_APPLICATION_VOIP (tuned for speech),
// 2049 = OPUS_APPLICATION_AUDIO (music), 2051 = RESTRICTED_LOWDELAY
var OPUS_ENCODER_APPLICATION = 2048;

// output capacity in milliseconds per decoder. must hold the largest incoming frame
// 60 ms covers the maximum Opus frame duration
var OPUS_DECODER_FRAME_CAPACITY_MS = 60;

// packet loss concealment synthesizes missing audio from decoder state
// only direct worklet playback uses it

// maximum concealed frames per gap. zero disables concealment
// long runs of synthesized audio distort speech
var OPUS_PLC_MAX_CONCEAL_FRAMES = 2;

// gaps above this size restart the decoder instead of inserting concealed audio
var OPUS_PLC_MAX_GAP_TO_CONCEAL = 25;

// each new transmission jumps the sequence by this amount and resets the encoder
// receivers reset their decoder at half this jump
// keep it above ordinary loss gaps and below 32768 for sequence wrap handling
var G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP = 1000;

// fallback mixer settings apply when AudioWorklet is unavailable

// fallback mixing interval. longer intervals save CPU but add playback delay
var OPUS_TICK_INTERVAL_MS = 20;

// a sender silent this long has its decoder returned to the pool for reuse
var OPUS_DECODER_IDLE_RELEASE_SECONDS = 5;

// consecutive late frames required to treat the sequence counter as restarted
var OPUS_STALE_FRAMES_BEFORE_RESYNC = 25;

// derived values. change the tuning constants above instead
var OPUS_DECODER_IDLE_TICKS_BEFORE_RELEASE = Math.round(OPUS_DECODER_IDLE_RELEASE_SECONDS * 1000 / OPUS_TICK_INTERVAL_MS);
var OPUS_HOUSEKEEPING_TICKS_PER_SECOND = Math.round(1000 / OPUS_TICK_INTERVAL_MS);

// a separate stereo encoder keeps stream audio independent of microphone codec state
var OPUS_STREAM_SOUND_APPLICATION = 2049;     // OPUS_APPLICATION_AUDIO
var OPUS_STREAM_SOUND_FRAME_DURATION_MS = 20; // one packet per 20 ms, the length of a capture chunk
var OPUS_STREAM_SOUND_BITRATE = 96000;        // bits per second for both channels together

// fixed Opus ABI constants
var OPUS_CTL_RESET_STATE = 4028; // OPUS_RESET_STATE from opus_defines.h
var OPUS_CTL_SET_BITRATE = 4002; // OPUS_SET_BITRATE_REQUEST from opus_defines.h


var custom_typeof = (function (global)
{
    var cache = {};
    return function (obj)
    {
        var key;
        return obj === null ? 'null' // null
            : obj === global ? 'global' // window in browser or global in nodejs
                : (key = typeof obj) !== 'object' ? key // basic: string, boolean, number, undefined, function
                    : obj.nodeType ? 'object' // DOM element
                        : cache[key = ({}).toString.call(obj)] // cached. date, regexp, error, object, array, math
                        || (cache[key] = key.slice(8, -1).toLowerCase()); // get XXXX from [object XXXX], and cache it
    };
}(this));



/**
 * @brief allocates an Opus encoder and resamplers for the supplied input rate
 *
 * @param number application -> the opus application mode (2048 voip, 2049 audio, 2051 restricted lowdelay)
 * @param number frameDuration -> the frame length in ms
 * @param number sampleRate -> the opus rate, 48000
 * @param number originalRate -> the microphone's rate. a resampler is built when it differs
 * @param number channels -> channels of the input
 * @param object params -> unused
 *
 * @return void a constructor, used with new
 */
function OpusEncoder(application, frameDuration, sampleRate, originalRate, channels, params)
{
    let err = 0;
    this.handle = this.bufPtr = this.outPtr = 0;
    this.originalRate = originalRate;
    this.sampleRate = sampleRate;
    this.channels = channels;
    this.frameSize = sampleRate * frameDuration / 1000;
    this.bufPos = 0;
    this.resampler = this.resampler_44100kHz_to_48000kHz = null;
    try
    {
        if (![8000, 12000, 16000, 24000, 48000].includes(sampleRate) || ![1, 2].includes(channels) || ![2.5, 5, 10, 20, 40, 60].includes(frameDuration))
        {
            throw new Error("Invalid Opus encoder format");
        }
        err = Module._malloc(4);
        this.handle = _opus_encoder_create(sampleRate, channels, application, err);
        if (!this.handle || Module.getValue(err, "i32") !== 0)
        {
            throw new Error("Opus encoder creation failed");
        }
        if (sampleRate !== originalRate)
        {
            this.resampler = new SpeexResampler(channels, originalRate, sampleRate);
        }
        this.resampler_44100kHz_to_48000kHz = new SpeexResampler(channels, 44100, 48000);
        const bufSize = 4 * this.frameSize * this.channels;
        const outSize = 1275 * 3 + 7;
        this.bufPtr = Module._malloc(bufSize);
        this.outPtr = Module._malloc(outSize);
        this.buf = Module.HEAPF32.subarray(this.bufPtr / 4, (this.bufPtr + bufSize) / 4);
        this.out = Module.HEAPU8.subarray(this.outPtr, this.outPtr + outSize);
    }
    catch (error)
    {
        this.destroy();
        throw error;
    }
    finally
    {
        Module._free(err);
    }
}

/**
 * @brief rebuilds cached views when WebAssembly memory has grown
 *
 * @return void
 */
OpusEncoder.prototype.refresh_heap_views_if_detached = function ()
{
    if (this.buf.buffer === Module.HEAPF32.buffer)
    {
        return;
    }

    this.buf = Module.HEAPF32.subarray(this.bufPtr / 4, (this.bufPtr + 4 * this.frameSize * this.channels) / 4);
    this.out = Module.HEAPU8.subarray(this.outPtr, this.outPtr + (1275 * 3 + 7));
}

/**
 * @brief clears buffered input and recreates active resamplers
 *
 * @return void
 */
OpusEncoder.prototype.reset = function ()
{

    if (this.buf && this.buf.length > 0)
    {
        this.buf.fill(0.0);
    }

    this.bufPos = 0;

    console.log("opus encoder reset");


    if (this.resampler)
    {
        let old_resampler = this.resampler;
        this.resampler = new SpeexResampler(this.channels, this.originalRate, this.sampleRate);
        old_resampler.destroy();
        console.log("old resampler destroyed");
    }

    if (this.resampler_44100kHz_to_48000kHz)
    {
        let old_resampler = this.resampler_44100kHz_to_48000kHz;
        this.resampler_44100kHz_to_48000kHz = new SpeexResampler(this.channels, 44100, 48000);
        old_resampler.destroy();
        console.log("old resampler destroyed");
    }

}

/**
 * @brief resamples microphone input when needed and encodes complete frames
 *
 * @param Float32Array samples -> mono pcm at the microphone rate
 *
 * @return array|undefined ArrayBuffer packets, or undefined when no packet completes or encoding fails
 */
OpusEncoder.prototype.encode = function (samples)
{
    // the microphone rate is fixed at construction, so this path uses one resampler

    var size;
    var ret;
    var result;
    var packets = [];

    if (this.resampler)
    {
        try
        {
            samples = this.resampler.process(samples);
        } catch (e)
        {
            console.log(e);
            return;
        }
    }

    this.refresh_heap_views_if_detached();

    while (samples && samples.length > 0)
    {
        size = Math.min(samples.length, this.buf.length - this.bufPos);
        this.buf.set(samples.subarray(0, size), this.bufPos);
        this.bufPos += size;
        samples = samples.subarray(size);
        if (this.bufPos == this.buf.length)
        {
            this.bufPos = 0;

            ret = _opus_encode_float(this.handle, this.bufPtr, this.frameSize, this.outPtr, this.out.byteLength);
            if (ret < 0)
            {
                console.log("encoder error");
                return;
            }
            result = (new Uint8Array(this.out.subarray(0, ret))).buffer;
            packets.push(result);
        }
    }
    if (packets.length > 0)
    {
        return packets;
    }
}

/**
 * @brief encodes decoded MP3 samples, converting 44100 Hz input to 48000 Hz
 *
 * @param Float32Array samples -> pcm from the mp3 decoder
 * @param number input_pcm_sample_rate -> the mp3's rate
 *
 * @return array|undefined ArrayBuffer packets, or undefined when no packet completes or encoding fails
 */
OpusEncoder.prototype.encode_mp3_chunk = function (samples, input_pcm_sample_rate)
{
    // only 44100 Hz input needs this resampler

    var size;
    var ret;
    var result;
    var packets = [];

    if (input_pcm_sample_rate == 44100)
    {
        try
        {
            samples = this.resampler_44100kHz_to_48000kHz.process(samples);
        } catch (e)
        {
            console.log(e);
            return;
        }
    }

    this.refresh_heap_views_if_detached();

    while (samples && samples.length > 0)
    {
        size = Math.min(samples.length, this.buf.length - this.bufPos);
        this.buf.set(samples.subarray(0, size), this.bufPos);
        this.bufPos += size;
        samples = samples.subarray(size);
        if (this.bufPos == this.buf.length)
        {
            this.bufPos = 0;

            ret = _opus_encode_float(this.handle, this.bufPtr, this.frameSize, this.outPtr, this.out.byteLength);
            if (ret < 0)
            {
                console.log("encoder error");
                return;
            }
            result = (new Uint8Array(this.out.subarray(0, ret))).buffer;
            packets.push(result);
        }
    }
    if (packets.length > 0)
    {
        return packets;
    }
}

/**
 * @brief sets the Opus bitrate in bits per second
 * the variadic control value is passed through a four byte heap allocation
 *
 * @param number bitrate -> bits per second
 *
 * @return void
 */
OpusEncoder.prototype.set_bitrate = function (bitrate)
{
    var pointer = Module._malloc(4);

    Module.HEAP32[pointer >> 2] = bitrate;

    try
    {
        _opus_encoder_ctl(this.handle, OPUS_CTL_SET_BITRATE, pointer);
    }
    catch (ctl_error)
    {
        console.log("opus encoder set bitrate failed: " + ctl_error);
    }

    Module._free(pointer);
}

/**
 * @brief frees the wasm encoder state and the resamplers
 *
 * @return void
 */
OpusEncoder.prototype.destroy = function ()
{
    if (this.handle)
    {
        _opus_encoder_destroy(this.handle);
    }
    Module._free(this.bufPtr);
    Module._free(this.outPtr);
    this.bufPtr = this.outPtr = 0;
    if (this.resampler)
    {
        this.resampler.destroy();
    }

    if (this.resampler_44100kHz_to_48000kHz)
    {
        this.resampler_44100kHz_to_48000kHz.destroy();
    }

    this.handle = null;
    this.buf = this.out = null;
    this.resampler = this.resampler_44100kHz_to_48000kHz = null;
}

/**
 * @brief allocates an Opus decoder and its packet and PCM buffers
 *
 * @param number sampleRate -> the opus rate, 48000
 * @param number channels -> output channels, 2 for the interleaved stereo everything downstream expects
 *
 * @return void a constructor, used with new
 */
function OpusDecoder(sampleRate, channels)
{
    let err = 0;
    this.handle = this.bufPtr = this.pcmPtr = 0;
    this.channels = channels;
    this.frameSize = sampleRate * OPUS_DECODER_FRAME_CAPACITY_MS / 1000;
    this.bufSize = 1275 * 3 + 7;
    this.pcmBufferSize = 4 * this.frameSize * channels;
    this.last_decoded_frame_size = 0;
    try
    {
        if (![8000, 12000, 16000, 24000, 48000].includes(sampleRate) || ![1, 2].includes(channels))
        {
            throw new Error("Invalid Opus decoder format");
        }
        err = Module._malloc(4);
        this.handle = _opus_decoder_create(sampleRate, channels, err);
        if (!this.handle || Module.getValue(err, "i32") !== 0)
        {
            throw new Error("Opus decoder creation failed");
        }
        this.bufPtr = Module._malloc(this.bufSize);
        this.pcmPtr = Module._malloc(this.pcmBufferSize);
        this.refresh_heap_views();
    }
    catch (error)
    {
        this.destroy();
        throw error;
    }
    finally
    {
        Module._free(err);
    }
}

/**
 * @brief rebuilds this codec instance's views of the current WebAssembly memory
 *
 * @return void
 */
OpusDecoder.prototype.refresh_heap_views = function()
{
    this.buf = Module.HEAPU8.subarray(this.bufPtr, this.bufPtr + this.bufSize);
    this.pcm = Module.HEAPF32.subarray(this.pcmPtr / 4, this.pcmPtr / 4 + this.frameSize * this.channels);
}

/**
 * @brief decodes one bounded Opus packet into this decoder's PCM buffer
 *
 * @param ArrayBuffer payload -> the opus packet
 *
 * @return number samples per channel produced, negative on an opus error, -1 for an oversize packet
 */
OpusDecoder.prototype.decode = function (payload)
{
    // the output capacity and returned sample count are per channel
    // decoded samples remain in this.pcm

    // reject oversized packets before copying into the fixed input buffer
    if (payload.byteLength > this.bufSize)
    {
        return -1;
    }

    this.buf.set(new Uint8Array(payload));
    var ret = _opus_decode_float(this.handle, this.bufPtr, payload.byteLength, this.pcmPtr, this.frameSize, 0);

    if (ret > 0)
    {
        this.last_decoded_frame_size = ret;
    }

    return ret;
}

/**
 * @brief synthesizes one missing frame using the last decoded frame size
 *
 * @return number samples per channel written into this.pcm, 0 when unable
 */
OpusDecoder.prototype.conceal_lost_frame = function ()
{
    if (this.last_decoded_frame_size <= 0)
    {
        return 0;
    }

    var ret = _opus_decode_float(this.handle, 0, 0, this.pcmPtr, this.last_decoded_frame_size, 0);

    if (ret < 0)
    {
        return 0;
    }

    return ret;
}

/**
 * @brief frees the wasm decoder state
 *
 * @return void
 */
OpusDecoder.prototype.destroy = function ()
{
    if (this.handle)
    {
        _opus_decoder_destroy(this.handle);
    }
    Module._free(this.bufPtr);
    Module._free(this.pcmPtr);
    this.bufPtr = this.pcmPtr = 0;
    this.handle = null;
    this.buf = null;
    this.pcm = null;
}

var IS_WORKER = !global.document && !!global.postMessage;
var IS_CURRENT_THREAD_WORKER = IS_WORKER && /blob:/i.test((global.location || {}).protocol);


/**
 * @brief creates a worker from moduleFactory with the requested THREAD_NAME
 *
 * @param string worker_name -> the name the worker runs under, "opus_decoder_worker" and the like
 *
 * @return Worker the new worker
 */
function audio_opus_glue__create_new_webworker_in_same_file(worker_name)
{
    console.log("trying to create webworker " + worker_name);
    let URL = global.URL || global.webkitURL || null;
    let code = moduleFactory.toString();

    // replace the worker name in the shared factory source

    let string_to_find = "var THREAD_NAME = "; // the first occurrence is this search string
    let first_occurence_index = code.indexOf(string_to_find);
    let replace_start_index = code.indexOf(string_to_find, first_occurence_index + 1);
    replace_start_index = replace_start_index + string_to_find.length;

    code = code.substring(0, replace_start_index) + "" + "\"" + worker_name + "" + code.substring(replace_start_index + worker_name.length, code.length);

    if (DBG_WORKER_BOOT_LOG)
    {
        console.log("[patcher] " + worker_name + " patched at " + replace_start_index);
    }


    let worker_url = URL.createObjectURL(new Blob(['(', code, ')();'], { type: 'text/javascript' }));
    let newly_created_worker = new global.Worker(worker_url);
    console.log("webworker created: " + worker_name);
    newly_created_worker.onmessage = dispatch__mainthread_onmessage;
    newly_created_worker.worker_name = worker_name;
    return newly_created_worker;
}


// worker replies are handled by dispatch__mainthread_onmessage in dispatch.js


/**
 * @brief handles encoder setup, reset and microphone or MP3 encoding
 *
 * @param object e -> the worker message event. E.data.value of "encode" must be a Float32Array
 *
 * @return void
 */
function audio_opus_glue__opus_encoder_worker_onmessage(e)
{
    if (e.data.type == "encode")
    {
        let opus_data_chunks = encoder.encode(e.data.value);

        // only post completed packets
        if (opus_data_chunks == null)
        {
            return;
        }

        global.postMessage({
            type: "opus_encoder_worker__encode_result",
            value: opus_data_chunks
        });
    }
    else if (e.data.type == "clear_opus_encoder_buffer")
    {
        // discard partial input so the next transmission starts clean
        if (encoder != null)
        {
            encoder.bufPos = 0;

            // rebuild the resampler to discard filter history from the previous transmission
            if (encoder.resampler != null)
            {
                let old_resampler = encoder.resampler;
                encoder.resampler = new SpeexResampler(encoder.channels, encoder.originalRate, encoder.sampleRate);
                old_resampler.destroy();
            }

            // reset the encoder with the sequence jump that resets receivers' decoders
            try
            {
                _opus_encoder_ctl(encoder.handle, OPUS_CTL_RESET_STATE);
            }
            catch (reset_err)
            {
                console.log("encoder reset_state failed: " + reset_err);
            }
        }
    }
    else if (e.data.type == "encode_mp3_chunk")
    {
        let opus_data_chunks = encoder.encode_mp3_chunk(e.data.value, e.data.mp3_sample_rate);

        // same guard as "encode": an incomplete frame returns nothing
        if (opus_data_chunks == null)
        {
            return;
        }

        global.postMessage({
            type: "opus_encoder_worker__encode_result",
            value: opus_data_chunks
        });
    }
    else if (e.data.type == "init")
    {
        let encoder_application_use = OPUS_ENCODER_APPLICATION; // VOIP use (see TUNABLES)
        let encoder_frame_duration = OPUS_ENCODER_FRAME_DURATION_MS; // Opus allows 2.5, 5, 10, 20, 40, or 60 ms
        let encoder_channels = 1;
        let encoder_output_samplerate = 48000; // if unsupported sample rate is used, Encoder wont construct
        let encoder_original_samplerate = e.data.sampleRate;
        encoder = new OpusEncoder(encoder_application_use, encoder_frame_duration, encoder_output_samplerate, encoder_original_samplerate, encoder_channels, 0);
    }
    else if (e.data.type == "destruct")
    {
        encoder.destroy();
    }
    else if (e.data.type == "init_stream_encoder")
    {
        // create a fresh encoder when stream audio starts
        if (stream_encoder != null)
        {
            stream_encoder.destroy();
        }
        stream_encoder = new OpusEncoder(OPUS_STREAM_SOUND_APPLICATION, OPUS_STREAM_SOUND_FRAME_DURATION_MS, 48000, e.data.sampleRate, 2, 0);
        stream_encoder.set_bitrate(OPUS_STREAM_SOUND_BITRATE);
    }
    else if (e.data.type == "encode_stream")
    {
        if (stream_encoder == null)
        {
            return;
        }

        let stream_opus_packets = stream_encoder.encode(e.data.value);

        // no whole packet completed by this chunk: nothing to send
        if (stream_opus_packets == null)
        {
            return;
        }

        global.postMessage({
            type: "opus_encoder_worker__stream_encode_result",
            value: stream_opus_packets
        });
    }
    else if (e.data.type == "destruct_stream_encoder")
    {
        if (stream_encoder != null)
        {
            stream_encoder.destroy();
            stream_encoder = null;
        }
    }
}


var opus_decoder_worker_interval;
var opus_decoder_worker_clients_opus_data = [];
var opus_decoder_worker_clients_opus_data_backbuffer = [];
var opus_decoder_worker_clients_opus_data_count = 0; // track the queued sender count separately
var opus_decoder_worker_current_channel_opus_client_ids = [];
var opus_decoder_worker_current_channel_opus_client_ids_map = new Map();

// allocate a decoder pool before audio starts. each active sender owns one slot
// reusing a slot resets its predictor without growing WebAssembly memory
var opus_decoder_pool = [];
var opus_decoder_pool_free_indices = [];
var opus_decoder_sender_map = new Map(); // sender client_id -> { pool_index, last_used_tick }
var opus_decoder_tick_counter = 0;
var opus_decoder_mix_scratch = null; // Float32Array(frameSize), created at init after the pool

// receive-side sequence telemetry (frames lost on the unordered/unreliable datachannel)
var opus_decoder_worker_lost_frame_count = 0;
var opus_decoder_worker_late_frame_count = 0;
var opus_decoder_worker_last_logged_lost_frame_count = 0;
var opus_decoder_worker_concealed_frame_count = 0;

// PLC settings (OPUS_PLC_MAX_CONCEAL_FRAMES / OPUS_PLC_MAX_GAP_TO_CONCEAL) are in the TUNABLES block at the top

// direct playback sends decoded PCM to the worklet through a MessageChannel
// null selects the fallback mixing timer
var opus_decoder_direct_worklet_port = null;

/**
 * @brief finds a sender's decoder or claims a pool slot
 * when the pool is full, reuse the sender slot that has been idle longest
 *
 * @param number sender_client_id -> the sender
 *
 * @return OpusDecoder|null the decoder, null when the pool is empty
 */
function audio_opus_glue__opus_decoder_worker_get_decoder_for_sender(sender_client_id)
{
    let mapping = opus_decoder_sender_map.get(sender_client_id);

    if (mapping != null)
    {
        mapping.last_used_tick = opus_decoder_tick_counter;
        return opus_decoder_pool[mapping.pool_index];
    }

    let pool_index = -1;

    if (opus_decoder_pool_free_indices.length > 0)
    {
        pool_index = opus_decoder_pool_free_indices.pop();
    }
    else
    {
        let oldest_tick = Infinity;
        let oldest_sender = null;

        for (const [sender, m] of opus_decoder_sender_map)
        {
            if (m.last_used_tick < oldest_tick)
            {
                oldest_tick = m.last_used_tick;
                oldest_sender = sender;
            }
        }

        if (oldest_sender == null)
        {
            console.warn("opus decoder pool exhausted: no free/idle decoder for new sender");
            return null;
        }

        pool_index = opus_decoder_sender_map.get(oldest_sender).pool_index;
        opus_decoder_sender_map.delete(oldest_sender);
    }

    // reset the previous occupant's predictor and frame size before reusing the slot
    _opus_decoder_ctl(opus_decoder_pool[pool_index].handle, OPUS_CTL_RESET_STATE);
    opus_decoder_pool[pool_index].last_decoded_frame_size = 0;

    opus_decoder_sender_map.set(sender_client_id, {
        pool_index: pool_index,
        last_used_tick: opus_decoder_tick_counter,
        last_sequence_number: null,
        consecutive_stale_count: 0
    });
    return opus_decoder_pool[pool_index];
}

/**
 * @brief releases decoder slots after their senders have been silent
 * the decoder state resets when its slot is reassigned
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_release_idle_decoders()
{
    for (const [sender, m] of opus_decoder_sender_map)
    {
        if (opus_decoder_tick_counter - m.last_used_tick > OPUS_DECODER_IDLE_TICKS_BEFORE_RELEASE)
        {
            opus_decoder_pool_free_indices.push(m.pool_index);
            opus_decoder_sender_map.delete(sender);
        }
    }
}


/**
 * @brief applies sequence checks and decodes a sender packet, optionally concealing gaps
 *
 * @param number sender_client_id -> the sender
 * @param object chunk_entry -> the opus bytes and their sequence number
 * @param boolean allow_plc -> true to patch concealed frames over a detected loss
 *
 * @return array|null fresh interleaved-stereo Float32Arrays (concealed frames before the real one), null when the chunk was dropped
 */
function audio_opus_glue__opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry, allow_plc)
{
    let sender_decoder = audio_opus_glue__opus_decoder_worker_get_decoder_for_sender(sender_client_id);
    let frames_to_conceal = 0;

    if (sender_decoder == null)
    {
        return null;
    }

    let sender_mapping = opus_decoder_sender_map.get(sender_client_id);

    // drop duplicate or late packets. a sustained run of late packets restarts the sequence
    if (chunk_entry.sequence_number != null && sender_mapping.last_sequence_number != null)
    {
        let sequence_delta = (chunk_entry.sequence_number - sender_mapping.last_sequence_number) & 0xffff;

        if (sequence_delta == 0)
        {
            return null; // duplicate frame
        }

        if (sequence_delta >= 0x8000)
        {
            opus_decoder_worker_late_frame_count = opus_decoder_worker_late_frame_count + 1;
            sender_mapping.consecutive_stale_count = sender_mapping.consecutive_stale_count + 1;

            // treat sustained stale packets as a restarted sender counter
            if (sender_mapping.consecutive_stale_count <= OPUS_STALE_FRAMES_BEFORE_RESYNC)
            {
                return null;
            }

            // reset decoder state with the restarted sequence
            _opus_decoder_ctl(sender_decoder.handle, OPUS_CTL_RESET_STATE);
            sender_decoder.last_decoded_frame_size = 0;
        }
        else if (sequence_delta - 1 >= (G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP >> 1))
        {
            // a deliberate sequence jump starts a new transmission, not a loss gap
            _opus_decoder_ctl(sender_decoder.handle, OPUS_CTL_RESET_STATE);
            sender_decoder.last_decoded_frame_size = 0;
        }
        else if (sequence_delta > 1)
        {
            let lost_now = sequence_delta - 1;

            if (lost_now > 250)
            {
                lost_now = 250; // keep the stat sane on an extreme burst
            }

            opus_decoder_worker_lost_frame_count = opus_decoder_worker_lost_frame_count + lost_now;

            // short gaps get concealed below, right before the real frame is decoded
            if (allow_plc && lost_now <= OPUS_PLC_MAX_GAP_TO_CONCEAL)
            {
                frames_to_conceal = Math.min(lost_now, OPUS_PLC_MAX_CONCEAL_FRAMES);
            }

            if (opus_decoder_worker_lost_frame_count - opus_decoder_worker_last_logged_lost_frame_count >= 100)
            {
                console.log("audio receive: " + opus_decoder_worker_lost_frame_count + " frames lost (" + opus_decoder_worker_concealed_frame_count + " concealed), " + opus_decoder_worker_late_frame_count + " late/duplicate so far");
                opus_decoder_worker_last_logged_lost_frame_count = opus_decoder_worker_lost_frame_count;
            }
        }
    }

    if (chunk_entry.sequence_number != null)
    {
        sender_mapping.last_sequence_number = chunk_entry.sequence_number;
        sender_mapping.consecutive_stale_count = 0;
    }

    let decoded_frames = [];

    // conceal missing frames before decoding the next real packet
    // the decoder predictor must advance in stream order
    for (let conceal_index = 0; conceal_index < frames_to_conceal; conceal_index++)
    {
        let concealed_sample_count = sender_decoder.conceal_lost_frame();

        if (concealed_sample_count <= 0)
        {
            break;
        }

        decoded_frames.push(new Float32Array(sender_decoder.pcm.subarray(0, concealed_sample_count * sender_decoder.channels)));
        opus_decoder_worker_concealed_frame_count = opus_decoder_worker_concealed_frame_count + 1;
    }

    let decoded_sample_count = sender_decoder.decode(chunk_entry.opus_chunk);

    if (decoded_sample_count <= 0)
    {
        if (decoded_frames.length == 0)
        {
            return null;
        }

        return decoded_frames;
    }

    // the count is per channel. the buffer contains interleaved stereo samples
    decoded_frames.push(new Float32Array(sender_decoder.pcm.subarray(0, decoded_sample_count * sender_decoder.channels)));
    return decoded_frames;
}

/**
 * @brief decodes one sender's audio and transfers real and concealed frames to the player worklet
 *
 * @param number sender_client_id -> the sender
 * @param object chunk_entry -> the opus bytes and their sequence number
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_decode_and_pipe(sender_client_id, chunk_entry)
{
    let decoded_frames = audio_opus_glue__opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry, true);

    if (decoded_frames == null)
    {
        return;
    }

    for (let frame_index = 0; frame_index < decoded_frames.length; frame_index++)
    {
        let decoded_pcm = decoded_frames[frame_index];

        opus_decoder_direct_worklet_port.postMessage({
            id: sender_client_id,
            pcm: decoded_pcm
        }, [decoded_pcm.buffer]);
    }
}

/**
 * @brief fallback (tick) mode: registers the sender for this 20 ms round and queues the chunk for the mixer
 *
 * @param number sender_client_id -> the sender
 * @param object chunk_entry -> the opus bytes and their sequence number
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_queue_chunk_for_tick(sender_client_id, chunk_entry)
{
    if (!opus_decoder_worker_current_channel_opus_client_ids.includes(sender_client_id))
    {
        opus_decoder_worker_current_channel_opus_client_ids.push(sender_client_id);
        opus_decoder_worker_current_channel_opus_client_ids.forEach((clientId, index) => {
            opus_decoder_worker_current_channel_opus_client_ids_map.set(clientId, index);
        });

        let new_client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(sender_client_id);
        opus_decoder_worker_clients_opus_data[new_client_index] = [];
    }

    let client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(sender_client_id);
    opus_decoder_worker_clients_opus_data[client_index].push(chunk_entry);
    opus_decoder_worker_clients_opus_data_count = opus_decoder_worker_clients_opus_data_count + 1;
}

/**
 * @brief releases idle decoder slots while direct playback bypasses the mixing tick
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_housekeeping_function()
{
    opus_decoder_tick_counter = opus_decoder_tick_counter + OPUS_HOUSEKEEPING_TICKS_PER_SECOND;
    audio_opus_glue__opus_decoder_worker_release_idle_decoders();
}

/**
 * @brief decodes queued sender packets, mixes them in sequence order and posts each playback block
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_interval_function()
{
    opus_decoder_tick_counter = opus_decoder_tick_counter + 1;

    // cheap periodic sweep: hand slots of long-silent senders back to the pool
    if ((opus_decoder_tick_counter & 63) == 0)
    {
        audio_opus_glue__opus_decoder_worker_release_idle_decoders();
    }

    if (opus_decoder_worker_clients_opus_data_count == 0)
    {
        return;
    }

    // sort sequenced packets in capture order across counter wrap
    // packets without sequence numbers keep arrival order
    for (let sender_index = 0; sender_index < opus_decoder_worker_current_channel_opus_client_ids.length; sender_index++)
    {
        let sender_chunks = opus_decoder_worker_clients_opus_data[sender_index];

        if (sender_chunks.length > 1 && sender_chunks[0].sequence_number != null)
        {
            let anchor = sender_chunks[0].sequence_number;

            sender_chunks.sort(function(a, b)
            {
                return ((a.sequence_number - anchor) & 0xffff) - ((b.sequence_number - anchor) & 0xffff);
            });
        }
    }

    // mix one round for each position in the longest sender queue


    let longestLength = 0;

    // find longest array that holds most ArrayBuffers (opus chunks)
    for (let client_index = 0; client_index < opus_decoder_worker_current_channel_opus_client_ids.length; client_index++)
    {
        let current_length = opus_decoder_worker_clients_opus_data[client_index].length;
        if (longestLength < current_length)
        {
            longestLength = current_length;
        }
    }

    // decode each sender independently, then sum and clamp the samples
    for (let ArrayBuffer_index = 0; ArrayBuffer_index < longestLength; ArrayBuffer_index++)
    {
        let mixed_sample_count = 0;

        opus_decoder_mix_scratch.fill(0);

        for (let client_index = 0; client_index < opus_decoder_worker_current_channel_opus_client_ids.length; client_index++)
        {
            // skip single_opus_chunk if client doesnt have element at that index
            if (ArrayBuffer_index >= opus_decoder_worker_clients_opus_data[client_index].length)
            {
                continue;
            }

            let sender_client_id = opus_decoder_worker_current_channel_opus_client_ids[client_index];
            let chunk_entry = opus_decoder_worker_clients_opus_data[client_index][ArrayBuffer_index];

            // the shared decoder drops invalid sequence entries
            // disable concealment here because extra frames would shift this sender within the mixed round
            let decoded_frames = audio_opus_glue__opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry, false);

            if (decoded_frames == null)
            {
                continue;
            }

            let decoded_pcm = decoded_frames[0];

            for (let k = 0; k < decoded_pcm.length; k++)
            {
                opus_decoder_mix_scratch[k] = opus_decoder_mix_scratch[k] + decoded_pcm[k];
            }

            if (decoded_pcm.length > mixed_sample_count)
            {
                mixed_sample_count = decoded_pcm.length;
            }
        }

        if (mixed_sample_count == 0)
        {
            continue;
        }

        // clamp summed samples to the output range and copy into a transferable buffer
        let pulse_code_modulation_bytes_for_webaudio = new Float32Array(mixed_sample_count);

        for (let k = 0; k < mixed_sample_count; k++)
        {
            let sample = opus_decoder_mix_scratch[k];
            pulse_code_modulation_bytes_for_webaudio[k] = sample > 1.0 ? 1.0 : (sample < -1.0 ? -1.0 : sample);
        }

        global.postMessage({
            type: "opus_decoder_worker__decode_result",
            value: pulse_code_modulation_bytes_for_webaudio
        }, [pulse_code_modulation_bytes_for_webaudio.buffer]);
    }

    opus_decoder_worker_clients_opus_data.length = 0;
    opus_decoder_worker_clients_opus_data_count = 0;
    opus_decoder_worker_current_channel_opus_client_ids.length = 0;
    opus_decoder_worker_current_channel_opus_client_ids_map.clear();
}

/**
 * @brief handles decoder input, pool state, playback ports and key updates
 *
 * @param object event -> the worker message event. Event.data.type picks the branch
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_onmessage(event)
{
    // voice frame: [4B sender client id][encrypted(2B sequence + opus)]
    if (event.data.type === "discard_sender_audio")
    {
        const sender_id = event.data.sender_id;
        const mapping = opus_decoder_sender_map.get(sender_id);
        if (mapping != null)
        {
            opus_decoder_pool_free_indices.push(mapping.pool_index);
            opus_decoder_sender_map.delete(sender_id);
        }
        const queue_index = opus_decoder_worker_current_channel_opus_client_ids.indexOf(sender_id);
        if (queue_index >= 0)
        {
            opus_decoder_worker_clients_opus_data[queue_index] = [];
        }
        if (opus_decoder_direct_worklet_port != null)
        {
            // the reset shares the PCM pipe, so earlier decoded frames cannot arrive after it
            opus_decoder_direct_worklet_port.postMessage({ type: "clear_sender", sender_id: sender_id });
        }
        return;
    }
    if (event.data.type == "mainthread__add_decrypted_data_to_opus_decoder")
    {
        // the main thread removed all encryption layers
        // layout: sender ID (4 bytes), sequence (2 bytes), then Opus. integers are little endian
        if (event.data.value.byteLength < 7)
        {
            return;
        }

        let dataView = new DataView(event.data.value);
        let extracted_client_id = dataView.getInt32(0, true);
        let chunk_entry = {
            sequence_number: dataView.getUint16(4, true),
            opus_chunk: event.data.value.slice(6)
        };

        // direct mode sends PCM to the worklet. fallback mode queues it for mixing
        if (opus_decoder_direct_worklet_port != null)
        {
            audio_opus_glue__opus_decoder_worker_decode_and_pipe(extracted_client_id, chunk_entry);
            return;
        }

        audio_opus_glue__opus_decoder_worker_queue_chunk_for_tick(extracted_client_id, chunk_entry);
    }
    else if (event.data.type == "mainthread__channel_keys_for_opus_decoder")
    {
        g_current_channel_keys = event.data.value;
        // console.log("mainthread__channel_keys_for_opus_decoder got channel keys", current_channel_keys);
    }
    else if (event.data.type == "use_direct_worklet_pipe")
    {
        // use the transferred port to send PCM directly to the player worklet
        // replace the mixing timer with decoder pool housekeeping
        opus_decoder_direct_worklet_port = event.data.port;

        clearInterval(opus_decoder_worker_interval);
        opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_housekeeping_function, 1000);

        // discard queued fallback audio before switching to direct playback
        opus_decoder_worker_clients_opus_data.length = 0;
        opus_decoder_worker_clients_opus_data_count = 0;
        opus_decoder_worker_current_channel_opus_client_ids.length = 0;
        opus_decoder_worker_current_channel_opus_client_ids_map.clear();

        console.log("opus decoder worker: direct worklet pipe active");
    }
    else if (event.data.type == "deep_idle_stop")
    {
        // android background idle: nothing plays, stop the periodic tick entirely
        clearInterval(opus_decoder_worker_interval);
        opus_decoder_worker_interval = null;
    }
    else if (event.data.type == "deep_idle_resume")
    {
        // restore whichever cadence matches the active pipeline
        // (worklet message channel -> slow housekeeping, fallback mixer -> fast tick)
        if (opus_decoder_worker_interval == null)
        {
            if (opus_decoder_direct_worklet_port != null)
            {
                opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_housekeeping_function, 1000);
            }
            else
            {
                opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_interval_function, OPUS_TICK_INTERVAL_MS);
            }
        }
    }
    else if (event.data.type == "init")
    {
        // fallback mixer tick (replaced by housekeeping once the worklet message channel is active)
        opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_interval_function, OPUS_TICK_INTERVAL_MS);

        // allocate the full stereo decoder pool before audio starts
        // libopus duplicates mono voice into stereo and preserves stereo bot audio
        for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
        {
            opus_decoder_pool.push(new OpusDecoder(48000, 2));
            opus_decoder_pool_free_indices.push(pool_index);
        }

        // later allocations may detach earlier views. refresh them after the pool is allocated
        for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
        {
            opus_decoder_pool[pool_index].refresh_heap_views();
        }

        opus_decoder_mix_scratch = new Float32Array(opus_decoder_pool[0].frameSize * opus_decoder_pool[0].channels);
    }
}
