// audio-opus-glue.js is embedded in template.html along with the other client files
// it is the opus codec side of audio: it turns microphone sound into network packets and packets back
// into sound, and runs inside the opus encoder and decoder workers (worker-entry.js picks the handler)
// audio.js and dispatch.js post to these workers, voice.js sends what they encode
// in this file a "frame" is one opus packet; in audio.js it is one moment of sound

// state private to this file
var encoder = null;


// OPUS AUDIO TUNABLES: each value below is safe to change on its own; numbers elsewhere in this file are load-bearing
// (decoder channel count 2 with stereo interleaving assumed downstream, sample rate 48000, OPUS_CTL_RESET_STATE 4028).
// the jitter buffer knobs under "AUDIO WORKLET TUNABLES" in audio.js matter more for felt delay than anything here

// how many people can be heard talking at once: one opus decoder per concurrent speaker (opus is stateful,
// streams cannot share one). all are allocated at worker start and never freed; a released decoder goes back
// to the pool. this is speakers-at-once, not channel size; a handful of simultaneous voices is already a mush
var OPUS_DECODER_POOL_SIZE = 16;

// microphone encode frame length. opus allows 2.5 / 5 / 10 / 20 / 40 / 60 ms.
// larger = fewer packets and better compression, but more latency and more
// audio lost per dropped packet. MUST be <= OPUS_DECODER_FRAME_CAPACITY_MS
var OPUS_ENCODER_FRAME_DURATION_MS = 40;

// opus encoder mode: 2048 = OPUS_APPLICATION_VOIP (tuned for speech),
// 2049 = OPUS_APPLICATION_AUDIO (music), 2051 = RESTRICTED_LOWDELAY
var OPUS_ENCODER_APPLICATION = 2048;

// decode output buffer size in ms of audio, per decoder: must hold the largest frame any sender might send
// (voice frames above, plus the server music bot's). 60 ms is the opus maximum, so it always fits; only lower
// this if you are sure no larger frames ever arrive
var OPUS_DECODER_FRAME_CAPACITY_MS = 60;

// --- packet loss concealment (PLC): when frames go missing, libopus can fabricate plausible fill audio
// from the decoder's state instead of leaving a gap or click. active only on the low-latency worklet path
// (the fallback mixer below never conceals)

// max fill frames invented per gap. concealment decays fast: 1-2 is
// transparent, 3+ starts to smear / sound robotic. set 0 to disable PLC
var OPUS_PLC_MAX_CONCEAL_FRAMES = 2;

// a gap wider than this many frames is treated as a dropout/reconnect rather
// than packet loss: the stream restarts clean instead of being prefixed with
// stale invented audio
var OPUS_PLC_MAX_GAP_TO_CONCEAL = 25;

// every talk-spurt start (press, song start) jumps the outgoing sequence by this much, and receivers scrub that
// sender's decoder at half this delta. must be well above any real in-mapping loss run (~160 frames before idle
// release) and under 32768 for the 16-bit wrap arithmetic; the sender's encoder is reset at the same boundary
var G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP = 1000;

// --- fallback mixer (only when AudioWorklet is unavailable) ---------------
// used on insecure contexts / old browsers. the worklet path ignores all of
// this and mixes on the audio clock instead.

// how often the fallback mixer wakes to decode+mix one round. raise toward
// 40-60 ms to save cpu on weak devices at the cost of latency; going much
// past ~60 ms makes playback choppy
var OPUS_TICK_INTERVAL_MS = 20;

// a sender silent this long has its decoder returned to the pool for reuse
var OPUS_DECODER_IDLE_RELEASE_SECONDS = 5;

// consecutive out-of-order "late" frames before we decide the sender
// restarted its sequence counter (a reconnect) and resync to it
var OPUS_STALE_FRAMES_BEFORE_RESYNC = 25;

// ---- derived from the knobs above; do not edit these directly ----
var OPUS_DECODER_IDLE_TICKS_BEFORE_RELEASE = Math.round(OPUS_DECODER_IDLE_RELEASE_SECONDS * 1000 / OPUS_TICK_INTERVAL_MS);
var OPUS_HOUSEKEEPING_TICKS_PER_SECOND = Math.round(1000 / OPUS_TICK_INTERVAL_MS);

// ---- fixed opus ABI constant, NOT a tunable ----
var OPUS_CTL_RESET_STATE = 4028; // OPUS_RESET_STATE from opus_defines.h


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
 * @brief an opus encoder on the wasm: the encoder state, a resampler when the microphone rate is not the opus rate, and the heap views the encode paths write into
 *
 * @param number application -> the opus application mode (2048 voip, 2049 audio, 2051 restricted lowdelay)
 * @param number frameDuration -> the frame length in ms
 * @param number sampleRate -> the opus rate, 48000
 * @param number originalRate -> the microphone's rate; a resampler is built when it differs
 * @param number channels -> channels of the input
 * @param object params -> unused
 *
 * @return void a constructor, used with new
 */
function OpusEncoder(application, frameDuration, sampleRate, originalRate, channels, params)
{
    var err;
    var bufSize;
    var outSize;

    this.originalRate = originalRate;
    this.resampler = null;
    this.resampler_44100kHz_to_48000kHz = null;
    
    this.sampleRate = sampleRate;
    this.bufPos = 0;
    err = Module._malloc(4);

    this.frameSize = sampleRate * frameDuration / 1000;
    this.channels = channels;

    // sample rate must be one of 8000, 12000, 16000, 24000, or 48000.

    this.handle = _opus_encoder_create(sampleRate, channels, application, err);

    if (this.handle == 0)
    {
        throw new Error('_opus_encoder_create fail ');
        return;
    }
    
    if (sampleRate != originalRate)
    {
        try
        {
            console.log("originalRate" + originalRate);
            console.log("sampleRate" + sampleRate);

            this.resampler = new SpeexResampler(channels, originalRate, sampleRate);
        }
        catch (e)
        {
            console.log("encoder new SpeexResampler(channels, originalRate, sampleRate) error");
            return;
        }
    }

    this.resampler_44100kHz_to_48000kHz = new SpeexResampler(channels, 44100, 48000);

    Module._free(err);
    bufSize = 4 * this.frameSize * this.channels;
    this.bufPtr = Module._malloc(bufSize);
    this.buf = Module.HEAPF32.subarray(this.bufPtr / 4, (this.bufPtr + bufSize) / 4);
    outSize = 1275 * 3 + 7;
    this.outPtr = Module._malloc(outSize);
    this.out = Module.HEAPU8.subarray(this.outPtr, this.outPtr + outSize);
}

/**
 * @brief re-derives the cached heap views when the wasm heap grew
 *        SpeexResampler.process reallocates its io buffers whenever a bigger chunk than any before
 *        arrives, which can trigger exactly that mid-run. called at the top of both encode paths;
 *        a no-op while the cached buffer still matches the live heap
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
 * @brief invalidates the audio still in the buffer and rebuilds the resampler
 *        added because leftover data remained after a push-to-talk; experimental, did not help
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
 * @brief encodes microphone samples: resamples when needed, fills whole frames and encodes each completed one
 *        whether resampling is needed is known at construction, from the microphone rate; done
 *        wrong, a voice comes out too high or too low
 *
 * @param Float32Array samples -> mono pcm at the microphone rate
 *
 * @return array|undefined the opus packets completed by these samples (Uint8Array each), undefined when the resampler failed
 */
OpusEncoder.prototype.encode = function (samples)
{
    // if resample is needed or not, is known right at the beginning, based on clients microphone
    // so only one resampler is used here, compared to encode_mp3_chunk function

    // if resampling is not done right, persons voice will be either high pitched, or too low

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
 * @brief encodes decoded mp3 samples, resampling 44100 Hz material to 48 kHz first, since every mp3 has its own rate
 *
 * @param Float32Array samples -> pcm from the mp3 decoder
 * @param number input_pcm_sample_rate -> the mp3's rate
 *
 * @return array|undefined the opus packets completed by these samples, undefined when the resampler failed
 */
OpusEncoder.prototype.encode_mp3_chunk = function (samples, input_pcm_sample_rate)
{
    // goal of this function is to make sure PCM is converted to 48kHz if it needs to be
    // problem is, each mp3 file has different sample rate, some are 44.1kHz, some are 48

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
 * @brief frees the wasm encoder state and the resamplers
 *
 * @return void
 */
OpusEncoder.prototype.destroy = function ()
{
    _opus_encoder_destroy(this.handle);
    if (this.resampler)
    {
        this.resampler.destroy();
    }

    if (this.resampler_44100kHz_to_48000kHz)
    {
        this.resampler_44100kHz_to_48000kHz.destroy();
    }

    this.handle = null;
    this.buf = null;
}

/**
 * @brief an opus decoder on the wasm: the decoder state and the heap views one packet and one decoded frame live in
 *
 * @param number sampleRate -> the opus rate, 48000
 * @param number channels -> output channels, 2 for the interleaved stereo everything downstream expects
 *
 * @return void a constructor, used with new
 */
function OpusDecoder(sampleRate, channels)
{
    this.channels = channels;
    var err = Module._malloc(4);
    this.handle = _opus_decoder_create(sampleRate, this.channels, err);
    var errNum = Module.getValue(err, "i32");
    Module._free(err);
    if (errNum != 0)
    {
        console.error("opus decoder creation failed, error code " + errNum);
        return;
    }

    this.frameSize = sampleRate * OPUS_DECODER_FRAME_CAPACITY_MS / 1000;
    var bufSize = 1275 * 3 + 7;
    var pcmSamples = this.frameSize * this.channels;

    this.bufSize = bufSize;
    this.bufPtr = Module._malloc(bufSize);
    this.buf = Module.HEAPU8.subarray(this.bufPtr, this.bufPtr + bufSize);


    this.pcmBufferSize = 4 * pcmSamples;

    // Module.HEAPF32.subarray creates a view for allocated buffer. Used when creating Float32Array later at some point
    this.pcmPtr = Module._malloc(this.pcmBufferSize);
    this.pcm = Module.HEAPF32.subarray(this.pcmPtr / 4, this.pcmPtr / 4 + pcmSamples);

    // samples per channel of the last successfully decoded real frame. packet loss
    // concealment must synthesize exactly one frame of the stream's real duration,
    // which is not known until the first frame of that stream has been decoded
    this.last_decoded_frame_size = 0;
}

/**
 * @brief re-derives the wasm-heap views from the current heap buffer
 *        the views cached at construction time detach if the emscripten heap grows during a later
 *        instance's allocation, so after building the whole decoder pool every member's views are
 *        refreshed once, when no further mallocs follow
 *
 * @return void
 */
OpusDecoder.prototype.refresh_heap_views = function()
{
    this.buf = Module.HEAPU8.subarray(this.bufPtr, this.bufPtr + this.bufSize);
    this.pcm = Module.HEAPF32.subarray(this.pcmPtr / 4, this.pcmPtr / 4 + this.frameSize * this.channels);
}

/**
 * @brief decodes one opus packet into this.pcm
 *        the payload is the audio bytes decrypted with the maintainer's channel key on this end;
 *        an oversize packet (beyond a valid opus packet's 1275*3+7 bytes) is refused, because it
 *        would throw a RangeError and take the whole decoder worker down
 *
 * @param ArrayBuffer payload -> the opus packet
 *
 * @return number samples per channel produced, negative on an opus error, -1 for an oversize packet
 */
OpusDecoder.prototype.decode = function (payload)
{
    // payload = audio bytes decrypted with the maintainer's channel key on this end. stock libopus signature
    // opus_decode_float(state, data, len, pcm_out, frame_size, decode_fec): frame_size is the capacity of pcm_out
    // in samples per channel, the return value how many were produced; decoded pcm is read from this.pcm afterwards

    // a valid opus packet never exceeds this.bufSize (1275*3+7); anything bigger would
    // make the set() below throw a RangeError and take the whole decoder worker down
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
 * @brief packet loss concealment: opus_decode_float with data=NULL and len=0 makes libopus synthesize one plausible frame from the predictor state instead of going silent
 *        possible once a real frame told us the stream's frame duration
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
    _opus_decoder_destroy(this.handle);
    this.handle = null;
    this.buf = null;
    this.pcm = null;
}

var IS_WORKER = !global.document && !!global.postMessage;
var IS_CURRENT_THREAD_WORKER = IS_WORKER && /blob:/i.test((global.location || {}).protocol);


/**
 * @brief creates one of the workers from this very page: the moduleFactory source is patched with the worker's name (THREAD_NAME) and loaded as a blob url
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

    // when creating new webworker within same file, get the string that represents code
    // alter the string, add variable to it with name of the worker

    let string_to_find = "var THREAD_NAME = "; // first offurence is this variable
    let first_occurence_index = code.indexOf(string_to_find);
    let replace_start_index = code.indexOf(string_to_find, first_occurence_index + 1);
    replace_start_index = replace_start_index + string_to_find.length;

    code = code.substring(0, replace_start_index) + "" + "\"" + worker_name + "" + code.substring(replace_start_index + worker_name.length, code.length);

    if (DBG_WORKER_BOOT_LOG) { console.log("[patcher] " + worker_name + " patched at " + replace_start_index); }


    let worker_url = URL.createObjectURL(new Blob(['(', code, ')();'], { type: 'text/javascript' }));
    let newly_created_worker = new global.Worker(worker_url);
    console.log("webworker created: " + worker_name);
    newly_created_worker.onmessage = dispatch__mainthread_onmessage;
    newly_created_worker.worker_name = worker_name;
    return newly_created_worker;
}


// dispatch__mainthread_onmessage (the main-thread worker-message dispatcher) lives in main.js


/**
 * @brief the opus encoder worker's message handler: "encode" and "encode_mp3_chunk" post the packets back, the rest manages the encoder (create, clear, destroy)
 *
 * @param object e -> the worker message event; e.data.value of "encode" must be a Float32Array
 *
 * @return void
 */
function audio_opus_glue__opus_encoder_worker_onmessage(e)
{
    if (e.data.type == "encode")
    {
        let opus_data_chunks = encoder.encode(e.data.value);

        // no full frame completed: nothing to send, and posting undefined
        // made the main thread throw on .length
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
        // drops the half-filled frame (it used to survive across push-to-talk sessions
        // and replay old speech on the next press)
        if (encoder != null)
        {
            encoder.bufPos = 0;

            // the mic-path resampler keeps a few ms of filter history; rebuild it so no
            // old samples bleed into the next talk (reset_mem is not exported from wasm)
            if (encoder.resampler != null)
            {
                let old_resampler = encoder.resampler;
                encoder.resampler = new SpeexResampler(encoder.channels, encoder.originalRate, encoder.sampleRate);
                old_resampler.destroy();
            }

            // codec predictor reset - one half of a pair with the receivers' spurt-start scrub
            // (triggered by the sequence jump); unpaired, spurt heads decode as a loud squelch
            try { _opus_encoder_ctl(encoder.handle, OPUS_CTL_RESET_STATE); } catch (reset_err) { console.log("encoder reset_state failed: " + reset_err); }
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
}


var opus_decoder_worker_interval;
var opus_decoder_worker_clients_opus_data = [];
var opus_decoder_worker_clients_opus_data_backbuffer = [];
var opus_decoder_worker_clients_opus_data_count = 0; // im keeping count in separate int varaible for simplier access
var opus_decoder_worker_current_channel_opus_client_ids = [];
var opus_decoder_worker_current_channel_opus_client_ids_map = new Map();

// the per-sender decoder pool: opus is stateful, so each concurrently audible sender needs its own decoder
// (interleaved streams through one corrupt each other's predictor). allocated once at init (a mid-run malloc can
// grow the wasm heap and detach the cached views), never freed: a release returns the slot, OPUS_RESET_STATE scrubs it
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

// PLC knobs (OPUS_PLC_MAX_CONCEAL_FRAMES / OPUS_PLC_MAX_GAP_TO_CONCEAL) live in the TUNABLES block up top

// direct mode: one end of a MessageChannel whose other end sits inside the player worklet.
// while set, chunks are decoded on arrival and their pcm goes straight to the worklet
// (per sender, no mixing here) - no 20 ms tick, no main-thread hop. null = tick/fallback mode
var opus_decoder_direct_worklet_port = null;

/**
 * @brief the sender's decoder, claiming a pool slot on first use
 *        on pool exhaustion the longest-idle sender's slot is stolen (that one stream restarts
 *        cleanly on its next frame, everyone else stays untouched)
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

    // scrub the previous occupant's predictor state in place - no malloc/free involved.
    // the frame-size hint dies with the state: concealment from a stranger's predictor
    // would synthesize the previous occupant's voice into the new sender's stream
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
 * @brief returns the slots of senders that have been silent for a while to the free list
 *        no state is touched here; the reset happens when the slot is reassigned
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
 * @brief decodes one chunk with that sender's pooled decoder under the per-sender sequence rules: duplicate and late drop, loss concealment, resync after a counter restart
 *        both the direct pipe and the 20 ms tick mixer use it
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

    // sequence handling: drop duplicates and late frames, conceal losses. a long run of
    // "late" frames means the sender restarted its counter (reconnect) - resync to it
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

            // a run of stale frames (~0.5 s of audio): not reordering, the sender's
            // counter restarted - fall through and resync to it
            if (sender_mapping.consecutive_stale_count <= OPUS_STALE_FRAMES_BEFORE_RESYNC)
            {
                return null;
            }

            // a restarted counter means the sender's codec restarted too: scrub, or its
            // fresh stream decodes through the dead session's predictor state
            _opus_decoder_ctl(sender_decoder.handle, OPUS_CTL_RESET_STATE);
            sender_decoder.last_decoded_frame_size = 0;
        }
        else if (sequence_delta - 1 >= (G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP >> 1))
        {
            // the sender's deliberate spurt-start jump (its encoder was reset at the same
            // boundary): scrub so both predictors begin from zero. not loss, so not counted
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

    // conceal BEFORE decoding the real frame: libopus extrapolates each synthetic frame
    // from the state as of the last good frame, and the real frame must be decoded after
    // them so the predictor continues in stream order
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

    // decode returns samples PER CHANNEL; the frame buffer holds interleaved stereo
    decoded_frames.push(new Float32Array(sender_decoder.pcm.subarray(0, decoded_sample_count * sender_decoder.channels)));
    return decoded_frames;
}

/**
 * @brief direct mode: decodes one sender's chunk immediately (no 20 ms tick latency) and transfers the pcm straight to the player worklet, which jitter-buffers per sender and mixes on the audio clock
 *        concealed frames ride the same pipe ahead of the real frame; to the worklet they are
 *        received audio filling the time the loss covered
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
 * @brief direct-pipe mode housekeeping: the mixing tick is gone, but idle decoder slots still need returning to the pool
 *        the tick counter advances by one tick-interval's worth per second so the "1 tick =
 *        OPUS_TICK_INTERVAL_MS" units of last_used_tick / IDLE_TICKS_BEFORE_RELEASE stay valid
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_housekeeping_function()
{
    opus_decoder_tick_counter = opus_decoder_tick_counter + OPUS_HOUSEKEEPING_TICKS_PER_SECOND;
    audio_opus_glue__opus_decoder_worker_release_idle_decoders();
}

/**
 * @brief the fallback mixer's 20 ms tick: orders each sender's queued chunks by sequence number (wrap-aware), decodes them with that sender's decoder, sums and clamps the frames and posts one mixed block per time step
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

    // order each sender's chunks by sequence number (wrap-aware around 65536) so frames the
    // unordered datachannel delivered scrambled are decoded in capture order. legacy chunks
    // without a sequence number keep arrival order
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

    // finf longest Array of ArrayBuffers. Who has it? the longest array length will be n times
    // loop n times, for each client, check if he has said index too, if he has it, merge it,


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

    // per time step: decode each sender's chunk with that sender's decoder (predictor state stays per-stream),
    // sum the frames in js, clamp to [-1,1], post one mixed block, the message the main thread always consumed.
    // decode is called with clear=1, so the shared frame buffer holds exactly this call's frame before accumulating
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

            // sequence rules and per-sender decode live in the shared helper (the direct pipe's too); null means
            // the chunk was dropped. plc is off here: the tick mixer aligns frames by queue position, so extra
            // concealed frames would shift this sender against the others mid-round
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

        // clamp-copy: summed streams can overshoot [-1,1]; the copy is freshly allocated so its
        // buffer is transferred instead of structure-cloned on the worker -> main hop
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
 * @brief the opus decoder worker's message handler
 *        a voice frame ([4B sender id][encrypted 2B sequence + opus]) is decrypted with the channel
 *        keys and decoded, through the direct pipe or the tick queue; the other types manage the
 *        pool, the pipe port and the keys
 *
 * @param object event -> the worker message event; event.data.type picks the branch
 *
 * @return void
 */
function audio_opus_glue__opus_decoder_worker_onmessage(event)
{
    // voice frame: [4B sender client id][encrypted(2B sequence + opus)]
    if (event.data.type == "mainthread__add_data_to_opus_decoder")
    {
        let dataView = new DataView(event.data.value);

        // clientid is always first 4 bytes of received chunk of bytes and is in every chunk of bytes
        let extracted_client_id = dataView.getInt32(0, true);

        let opus_ArrayBuffer_encrypted = event.data.value.slice(4);
        let decrypted_bytes = keys__decrypt_data_with_aes_keys(g_current_channel_keys, opus_ArrayBuffer_encrypted);

        // after decryption: [2B sequence little endian][opus]. too-short frames are dropped
        if (decrypted_bytes == null || decrypted_bytes.length < 3)
        {
            return;
        }

        let chunk_entry = {
            sequence_number: decrypted_bytes[0] | (decrypted_bytes[1] << 8),
            opus_chunk: decrypted_bytes.subarray(2)
        };

        // direct mode decodes right now and pipes to the worklet; tick mode queues for the mixer
        if (opus_decoder_direct_worklet_port != null)
        {
            audio_opus_glue__opus_decoder_worker_decode_and_pipe(extracted_client_id, chunk_entry);
            return;
        }

        audio_opus_glue__opus_decoder_worker_queue_chunk_for_tick(extracted_client_id, chunk_entry);
    }
    else if (event.data.type == "mainthread__add_data_to_opus_decoder_musicbot")
    {
        let dataView = new DataView(event.data.value);
        let extracted_client_id = dataView.getInt32(0, true);
        let chunk_entry = null;

        if (extracted_client_id == -2)
        {
            // legacy format from an old server binary: [4B -2][opus], no sequence number
            chunk_entry = {
                sequence_number: null,
                opus_chunk: event.data.value.slice(4)
            };
        }
        else
        {
            // new format: [4B bot client id][2B sequence little endian][opus]
            if (event.data.value.byteLength < 7)
            {
                return;
            }

            chunk_entry = {
                sequence_number: dataView.getUint16(4, true),
                opus_chunk: event.data.value.slice(6)
            };
        }

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
        // the main thread transferred one end of a MessageChannel whose other end sits inside the
        // player worklet: from here on, decode on arrival and pipe per-sender pcm directly - the
        // mixing tick and the main-thread hop are gone. the interval becomes slow housekeeping
        opus_decoder_direct_worklet_port = event.data.port;

        clearInterval(opus_decoder_worker_interval);
        opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_housekeeping_function, 1000);

        // drop anything the tick queue still holds; the pipe owns playback from here
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
        // (worklet pipe -> slow housekeeping, fallback mixer -> fast tick)
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
        // fallback mixer tick (replaced by housekeeping once the worklet pipe activates)
        opus_decoder_worker_interval = setInterval(audio_opus_glue__opus_decoder_worker_interval_function, OPUS_TICK_INTERVAL_MS);

        // the whole per-sender decoder pool is allocated up front: every wasm malloc happens here, before any
        // audio flows, so the heap never grows mid-run under the cached views. decoders are stereo (a decoder's
        // channel count is independent of the packet's): stereo bot packets stay stereo, mono voice is duplicated by libopus
        for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
        {
            opus_decoder_pool.push(new OpusDecoder(48000, 2));
            opus_decoder_pool_free_indices.push(pool_index);
        }

        // allocating a later pool member may have grown the heap and detached the views the
        // earlier members cached at construction - re-derive them all now that mallocs are done
        for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
        {
            opus_decoder_pool[pool_index].refresh_heap_views();
        }

        opus_decoder_mix_scratch = new Float32Array(opus_decoder_pool[0].frameSize * opus_decoder_pool[0].channels);
    }
}
