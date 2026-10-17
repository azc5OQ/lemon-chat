// audio.js is embedded in template.html along with the other client files
// it is the audio engine: it moves sound around (microphone in, speakers out, buffering and timing)
// and never compresses anything; audio-opus-glue.js does the opus packets inside the workers
// it holds the worklet processors and their ScriptProcessorNode fallbacks, the playback and capture
// graphs, the push-to-talk gate and the after-idle rebuild; voice.js and dispatch.js call it, and video.js asks it for
// the stereo capture node of a video stream's sound and plays that sound through a reserved playback buffer

// the words used here: every sender has its own playback buffer; its
// target is the amount of audio it collects before it starts playing (an underrun, the buffer running empty,
// raises the target; dropping single frames from an overfull buffer lowers the delay again); a frame is one
// stereo sample pair (two floats), a quantum is one 128-frame tick of the audio engine, interleaved means left,
// right, left, right in one list

// state private to this file
var player = null;

var audio_player_worklet_node = null;

var is_audio_worklet_player_active = false;

var audio_worklet_module_promise = null; // set while/after the worklet module loads; null = no worklet support

var is_microphone_worklet_active = false;

var PREMIXED_AUDIO_LANE_ID = 9999999; // worklet playback_buffer id for already-mixed audio (matches PREMIXED_LANE_ID in the processor)

// the sender id under which the sound of a video stream is decoded and played (video.js hands the packets to the decoder
// worker under it): no client can have it, client ids are small numbers. matches STREAM_SOUND_PLAYBACK_BUFFER_ID in the processor
var VIDEO_STREAM_SOUND_SENDER_ID = 9999998;

var audio_queue = {
    buffer: new Float32Array(0),

    // AUDIO TUNABLE (fallback path only; the worklet path uses the "AUDIO WORKLET TUNABLES" jitter buffer):
    // the hard cap on the ScriptProcessor's queue, ~300 ms of interleaved stereo. beyond it the oldest
    // samples drop, so playback snaps back to near-real-time after gc pauses, throttling or a rate mismatch
    max_buffered_samples: 28800,

    /**
     * @brief appends decoded samples to the fallback playback queue
     *        beyond max_buffered_samples the oldest samples are dropped, so playback snaps back to
     *        near-real-time instead of lagging forever
     *
     * @param Float32Array newAudio -> interleaved stereo samples
     *
     * @return void
     */
    write: function (newAudio)
    {
        var currentQLength = this.buffer.length;
        var newBuffer = new Float32Array(currentQLength + newAudio.length);
        newBuffer.set(this.buffer, 0);
        newBuffer.set(newAudio, currentQLength);

        if (newBuffer.length > this.max_buffered_samples)
        {
            newBuffer = newBuffer.subarray(newBuffer.length - this.max_buffered_samples);
        }

        this.buffer = newBuffer;
    },

    /**
     * @brief empties the fallback playback queue
     *
     * @return void
     */
    clear: function ()
    {
        this.buffer = g_silence;
    },

    /**
     * @brief takes the next samples off the front of the fallback playback queue
     *
     * @param number nSamples -> how many floats to take
     *
     * @return Float32Array the samples, fewer when the queue is empty
     */
    read: function (nSamples)
    {
        var samplesToPlay = this.buffer.subarray(0, nSamples);
        this.buffer = this.buffer.subarray(nSamples, this.buffer.length);
        return samplesToPlay;
    },

    /**
     * @brief how many floats the fallback playback queue holds
     *
     * @return number the queued floats
     */
    length: function ()
    {
        return this.buffer.length;
    }
};

/**
 * @brief builds a brand new AudioContext and re-wires playback and (if present) the microphone onto it
 *        resuming a context whose HAL android reset is what corrupted both directions
 *
 * @return void
 */
function audio__rebuild_audio_graph_after_idle()
{
    let old_context = g_audio_context;
    let old_sample_rate = (old_context != null) ? old_context.sampleRate : 48000;

    try
    {
        g_audio_context = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
    }
    catch (sample_rate_hint_not_supported)
    {
        g_audio_context = new (window.AudioContext || window.webkitAudioContext)();
    }

    // playback graph: gain -> speakers, then the player node (worklet or script processor)
    g_audio_player_gain_node = g_audio_context.createGain();
    g_audio_player_gain_node.connect(g_audio_context.destination);
    audio__create_audio_player_output();

    // microphone: the MediaStream survives a context swap, the nodes do not
    if (g_local_audio_stream != null && g_microphone_recorder != null)
    {
        // audio__create_microphone_capture_node builds a fresh capture node on the new context
        g_microphone_recorder = null;
        g_audio_input = g_audio_context.createMediaStreamSource(g_local_audio_stream);
        g_audio_recorder_gain_node = g_audio_context.createGain();
        audio__create_microphone_capture_node();
    }

    // a fresh context may come up at a different device rate; the decode resampler and
    // both codec workers are bound to it, so re-init them on a change
    if (g_audio_context.sampleRate !== old_sample_rate)
    {
        g_opus_decoding_sampler = new SpeexResampler(1, 48000, g_audio_context.sampleRate);
        g_opus_decoder_worker.postMessage({ type: "init", sampleRate: g_audio_context.sampleRate });

        if (g_microphone_recorder != null)
        {
            g_opus_encoder_worker.postMessage({ type: "init", sampleRate: g_audio_context.sampleRate });
        }
    }

    // the old context is no longer used; close() may reject on an already closed one, both fine
    if (old_context != null)
    {
        try { old_context.close().catch(function() {}); } catch (e) {}
    }

    console.log("audio graph rebuilt on a fresh context, sampleRate " + g_audio_context.sampleRate);
}

/**
 * @brief the AudioWorklet processors, the player and the recorder
 *        never called on the main thread: its source is stringified into a blob module and runs on
 *        the audio rendering thread, where a busy main thread cannot stall it (the ScriptProcessorNode
 *        it replaces ran on the main thread and stuttered whenever the page was busy)
 *
 * @return void
 */
function audio__lemon_audio_worklet_scope()
{
    // AudioWorkletProcessor is a class the browser only defines inside the audio worklet scope,
    // never on the main thread - another reason this function must not be called directly
    class LemonPlayerProcessor extends AudioWorkletProcessor
    {
        constructor()
        {
            super();

            // per-sender playback buffers: sender id -> its own buffer of interleaved stereo that is written in a
            // circle (when the end is reached, writing continues at the start), with jitter buffering. a buffer
            // collects audio up to a target depth before it plays, raises the target
            // when it runs empty and lowers it again (dropping the excess) once the flow is stable; the
            // buffers are mixed here, on the audio clock
            this.playback_buffers = new Map();
            this.pending_playback_buffer_gains = new Map(); // volume set before the playback_buffer's first pcm arrived

            // AUDIO WORKLET TUNABLES: the per-sender jitter buffer, which sets felt latency vs. smoothness on
            // the worklet path. TARGET_START is the delay each voice adds before it starts playing: lower it for
            // less latency, raise it if choppy. values are interleaved-stereo floats (1 ms = 96, 60 ms = 5760)
            this.RING_SIZE = 65536;          // interleaved floats per playback_buffer (~680 ms), power of two
            this.RING_MASK = 65535;          // "index & mask" wraps cheaply (replaces % RING_SIZE)
            this.RING_HARD_CAP = 57600;      // ~600 ms; beyond this the oldest samples are dropped
            this.TARGET_START = 5760;        // initial jitter target: 60 ms of interleaved stereo
            this.TARGET_MIN = 5760;          // never shrink below 60 ms
            this.TARGET_MAX = 28800;         // never grow beyond 300 ms
            this.TARGET_GROW_STEP = 2880;    // +30 ms of cushion per underrun
            this.TARGET_SHRINK_STEP = 960;   // -10 ms per stable period
            this.SHRINK_AFTER_QUANTA = 1875; // one stable period = ~5 s (375 quanta per second)
            this.SLIP_EXCESS = 1920;         // buffered > target + 20 ms -> slip samples out
            this.FADE_FRAMES = 192;          // 4 ms fade at playback_buffer (re)start and near-empty edges
            this.PREMIXED_LANE_ID = 9999999; // playback_buffer for already-mixed audio from the main thread
            this.LANE_IDLE_QUANTA = 3750;    // ~10 s without new data -> playback_buffer deleted
            this.TALK_END_QUANTA = 188;      // ~500 ms without new data -> push-to-talk was released

            // music bot buffers: the server streams a bot MUSIC_BOT_LEAD_MS (3 s) ahead of playback, so
            // its buffer holds that much on purpose - a large, fixed amount instead of the low-latency
            // voice rules. BOT_TARGET must match the server's lead
            this.BOT_TARGET = 288000;        // 3 s held before and during playback
            this.BOT_TARGET_MAX = 336000;    // grows to 3.5 s at most after underruns
            this.BOT_RESUME_DEPTH = 96000;   // after a start or an underrun, play again at 1 s of audio
            this.BOT_SLIP_EXCESS = 48000;    // slip only above target + 500 ms (music must not be time-squeezed)
            this.BOT_RING_SIZE = 524288;     // ~5.4 s, power of two
            this.BOT_RING_MASK = 524287;
            this.BOT_RING_HARD_CAP = 432000; // ~4.5 s; beyond this the oldest samples are dropped

            // the sound of a video stream (video.js): one buffer under a reserved sender id, deeper than a voice so it survives
            // the bursts of the video connection (a keyframe queues ahead of the sound there), and never time-squeezed like
            // music. its depth is the picture hold of the viewer (VIDEO_SOUND_PICTURE_HOLD_MS in video.js): picture and
            // sound then wait about equally long
            this.STREAM_SOUND_PLAYBACK_BUFFER_ID = 9999998;
            this.STREAM_SOUND_TARGET = 28800;        // 300 ms held before playback starts, and again after an underrun
            this.STREAM_SOUND_TARGET_MAX = 57600;    // grows to 600 ms at most after underruns
            this.STREAM_SOUND_SLIP_EXCESS = 28800;   // slip only above target + 300 ms
            this.STREAM_SOUND_RING_HARD_CAP = 96000; // ~1 s; beyond this the oldest samples are dropped
            this.music_bot_senders = new Set(); // announced by the main thread from the client list

            this.quantum_counter = 0;

            this.port.onmessage = (event) =>
            {
                // "clear" = channel switch: throw away everything buffered in every buffer
                if (event.data === "clear")
                {
                    this.playback_buffers.clear();
                    return;
                }

                // which senders are music bots, so their buffers get the large fixed depth on creation
                if (event.data.type === "music_bot_senders")
                {
                    this.music_bot_senders = new Set(event.data.ids);
                    return;
                }

                // a diagnostic snapshot of every buffer, answered on the same port
                if (event.data.type === "playback_buffer_stats")
                {
                    let stats = [];
                    for (const [sender_id, playback_buffer] of this.playback_buffers)
                    {
                        stats.push({ sender_id: sender_id, is_music_bot: playback_buffer.is_music_bot, state: playback_buffer.state,
                                     available_ms: Math.round(playback_buffer.available / 96), target_ms: Math.round(playback_buffer.target / 96) });
                    }
                    this.port.postMessage({ type: "playback_buffer_stats", playback_buffers: stats });
                    return;
                }

                // the decoder worker's end of the direct message channel: per-sender pcm arrives here
                // from the worker thread without ever touching the main thread
                if (event.data.type === "pipe")
                {
                    event.data.port.onmessage = (pipe_event) =>
                    {
                        this.playback_buffer_write(pipe_event.data.id, pipe_event.data.pcm);
                    };
                    return;
                }

                // per-user playback volume, applied at the mix below
                if (event.data.type === "gain")
                {
                    let playback_buffer = this.playback_buffers.get(event.data.sender_id);
                    if (playback_buffer != null)
                    {
                        playback_buffer.gain = event.data.value;
                    }
                    // remember it for a buffer that does not exist yet (user preset before talking)
                    this.pending_playback_buffer_gains.set(event.data.sender_id, event.data.value);
                    return;
                }

                // pre-mixed pcm from the main thread (the flush of audio buffered while this
                // module was still loading); plays through its own low-latency buffer
                if (event.data.type === "pcm")
                {
                    this.playback_buffer_write(event.data.sender_id, event.data.pcm);
                }
            };
        }

        // appends one pcm chunk to a sender's buffer, creating the buffer on first use
        playback_buffer_write(sender_id, chunk)
        {
            let playback_buffer = this.playback_buffers.get(sender_id);

            if (playback_buffer == null)
            {
                let is_music_bot = this.music_bot_senders.has(sender_id);
                let is_premixed = (sender_id === this.PREMIXED_LANE_ID);
                let is_stream_sound = (sender_id === this.STREAM_SOUND_PLAYBACK_BUFFER_ID);

                // four kinds of buffer: the pre-mixed flush (tiny), a voice (low latency, adaptive), a music bot (large
                // fixed depth filled by the server's lead) and the sound of a video stream (medium fixed depth)
                playback_buffer = {
                    ring: new Float32Array((is_music_bot || is_stream_sound) ? this.BOT_RING_SIZE : this.RING_SIZE),
                    ring_mask: (is_music_bot || is_stream_sound) ? this.BOT_RING_MASK : this.RING_MASK,
                    hard_cap: is_music_bot ? this.BOT_RING_HARD_CAP : (is_stream_sound ? this.STREAM_SOUND_RING_HARD_CAP : this.RING_HARD_CAP),
                    read_index: 0,
                    write_index: 0,
                    available: 0,
                    state: 0, // 0 = prebuffering, 1 = playing
                    target: is_premixed ? 960 : (is_music_bot ? this.BOT_TARGET : (is_stream_sound ? this.STREAM_SOUND_TARGET : this.TARGET_START)),
                    target_min: is_premixed ? 960 : (is_music_bot ? this.BOT_TARGET : (is_stream_sound ? this.STREAM_SOUND_TARGET : this.TARGET_MIN)),
                    target_max: is_music_bot ? this.BOT_TARGET_MAX : (is_stream_sound ? this.STREAM_SOUND_TARGET_MAX : this.TARGET_MAX),
                    slip_excess: is_music_bot ? this.BOT_SLIP_EXCESS : (is_stream_sound ? this.STREAM_SOUND_SLIP_EXCESS : this.SLIP_EXCESS),
                    is_music_bot: is_music_bot,
                    fade_in_remaining: 0,
                    stable_quanta: 0,
                    last_data_quantum: this.quantum_counter,
                    gain: this.pending_playback_buffer_gains.has(sender_id) ? this.pending_playback_buffer_gains.get(sender_id) : 1.0
                };
                this.playback_buffers.set(sender_id, playback_buffer);
            }

            playback_buffer.last_data_quantum = this.quantum_counter;

            // hard cap: drop the oldest first - delay that piled up is never recovered by itself
            if (playback_buffer.available + chunk.length > playback_buffer.hard_cap)
            {
                let drop = playback_buffer.available + chunk.length - playback_buffer.hard_cap;
                if (drop > playback_buffer.available)
                {
                    drop = playback_buffer.available;
                }
                playback_buffer.read_index = (playback_buffer.read_index + drop) & playback_buffer.ring_mask;
                playback_buffer.available = playback_buffer.available - drop;
            }

            for (let i = 0; i < chunk.length; i++)
            {
                playback_buffer.ring[playback_buffer.write_index] = chunk[i];
                playback_buffer.write_index = (playback_buffer.write_index + 1) & playback_buffer.ring_mask;
            }
            playback_buffer.available = playback_buffer.available + chunk.length;
        }

        // called by the audio engine every render quantum (128 frames, ~2.7 ms at 48 kHz);
        // output buffers arrive zeroed, the playback buffers are summed into them, then soft-limited
        process(inputs, outputs)
        {
            let left_channel = outputs[0][0];
            let right_channel = outputs[0].length > 1 ? outputs[0][1] : null;
            let frame_count = left_channel.length;

            this.quantum_counter = this.quantum_counter + 1;

            for (const [sender_id, playback_buffer] of this.playback_buffers)
            {
                // sender went silent long ago: free the buffer (the next talk starts a new one)
                if (this.quantum_counter - playback_buffer.last_data_quantum > this.LANE_IDLE_QUANTA)
                {
                    this.playback_buffers.delete(sender_id);
                    continue;
                }

                // a buffer still collecting with no new audio for a while: the key was released, so
                // what remains in the buffer is the end of the last transmission and the raised target is outdated
                if (playback_buffer.state === 0 && this.quantum_counter - playback_buffer.last_data_quantum > this.TALK_END_QUANTA)
                {
                    playback_buffer.available = 0;
                    playback_buffer.read_index = playback_buffer.write_index;
                    playback_buffer.target = playback_buffer.target_min;
                    playback_buffer.stable_quanta = 0;
                }

                // prebuffering: hold until the jitter target is reached, then start with a fade-in.
                // a music bot buffer starts (and restarts) once a second of audio is in; the server's
                // lead fills the rest of its depth while it already plays
                if (playback_buffer.state === 0)
                {
                    if (playback_buffer.available < (playback_buffer.is_music_bot ? this.BOT_RESUME_DEPTH : playback_buffer.target))
                    {
                        continue;
                    }
                    playback_buffer.state = 1;
                    playback_buffer.fade_in_remaining = this.FADE_FRAMES;
                }

                // stable long enough: shrink the target one step back toward the minimum (a music
                // bot's depth is fixed, it never shrinks)
                playback_buffer.stable_quanta = playback_buffer.stable_quanta + 1;
                if (playback_buffer.stable_quanta >= this.SHRINK_AFTER_QUANTA)
                {
                    playback_buffer.stable_quanta = 0;
                    if (playback_buffer.is_music_bot == false && playback_buffer.target > playback_buffer.target_min)
                    {
                        playback_buffer.target = playback_buffer.target - this.TARGET_SHRINK_STEP;
                    }
                }

                // frame dropping: while the buffered depth exceeds target + the buffer's allowance, silently drop
                // one frame per quantum (~0.8% time compression) - drains built-up delay with no click
                if (playback_buffer.available > playback_buffer.target + playback_buffer.slip_excess)
                {
                    playback_buffer.read_index = (playback_buffer.read_index + 2) & playback_buffer.ring_mask;
                    playback_buffer.available = playback_buffer.available - 2;
                }

                for (let i = 0; i < frame_count; i++)
                {
                    if (playback_buffer.available < 2)
                    {
                        // underrun: back to collecting with a larger target; the fade-out
                        // below already brought the level to ~zero, so no click
                        playback_buffer.state = 0;
                        playback_buffer.stable_quanta = 0;
                        if (playback_buffer.target < playback_buffer.target_max)
                        {
                            playback_buffer.target = playback_buffer.target + this.TARGET_GROW_STEP;
                        }
                        break;
                    }

                    let gain = playback_buffer.gain;

                    // fade-in over the first few ms after a (re)start
                    if (playback_buffer.fade_in_remaining > 0)
                    {
                        gain = gain * (1 - playback_buffer.fade_in_remaining / this.FADE_FRAMES);
                        playback_buffer.fade_in_remaining = playback_buffer.fade_in_remaining - 1;
                    }

                    // fade-out toward an approaching empty buffer, so an empty buffer never clicks
                    let frames_left = playback_buffer.available >> 1;
                    if (frames_left < this.FADE_FRAMES)
                    {
                        gain = gain * (frames_left / this.FADE_FRAMES);
                    }

                    // one output frame consumes two samples from the buffer: left then right (interleaved)
                    left_channel[i] = left_channel[i] + playback_buffer.ring[playback_buffer.read_index] * gain;
                    playback_buffer.read_index = (playback_buffer.read_index + 1) & playback_buffer.ring_mask;

                    let right_sample = playback_buffer.ring[playback_buffer.read_index];
                    playback_buffer.read_index = (playback_buffer.read_index + 1) & playback_buffer.ring_mask;
                    playback_buffer.available = playback_buffer.available - 2;

                    if (right_channel != null)
                    {
                        right_channel[i] = right_channel[i] + right_sample * gain;
                    }
                }
            }

            // soft limiter: |x| <= 0.85 passes untouched, louder sums get squeezed smoothly toward
            // 1.0 instead of hard-clipping into crackle when several loud senders overlap
            for (let i = 0; i < frame_count; i++)
            {
                let left_sample = left_channel[i];
                if (left_sample > 0.85)
                {
                    left_channel[i] = 0.85 + 0.15 * Math.tanh((left_sample - 0.85) / 0.15);
                }
                else if (left_sample < -0.85)
                {
                    left_channel[i] = -0.85 - 0.15 * Math.tanh((-left_sample - 0.85) / 0.15);
                }

                if (right_channel != null)
                {
                    let right_sample = right_channel[i];
                    if (right_sample > 0.85)
                    {
                        right_channel[i] = 0.85 + 0.15 * Math.tanh((right_sample - 0.85) / 0.15);
                    }
                    else if (right_sample < -0.85)
                    {
                        right_channel[i] = -0.85 - 0.15 * Math.tanh((-right_sample - 0.85) / 0.15);
                    }
                }
            }

            return true; // keep the processor alive
        }
    }

    // the microphone capture processor, the player's mirror image: the mic chain is connected into this node,
    // every 128-frame quantum is appended to a chunk of the size the old ScriptProcessorNode delivered, and
    // full chunks go to the main thread for the encoder worker. "start"/"stop" port messages gate it (push-to-talk)
    class LemonRecorderProcessor extends AudioWorkletProcessor
    {
        constructor(options)
        {
            super();
            // chunk size comes from the main thread (g_audio_config.codec.bufferSize) so the opus
            // encoder worker sees identical input on both capture paths
            this.chunk_size = (options && options.processorOptions && options.processorOptions.chunk_size) ? options.processorOptions.chunk_size : 8192;
            // 1 = the microphone's mono capture (the first channel only); 2 = the stereo capture of a video stream's sound,
            // left and right interleaved in the chunk, so chunk_size must be even there
            this.channel_count = (options && options.processorOptions && options.processorOptions.channel_count == 2) ? 2 : 1;
            this.chunk = new Float32Array(this.chunk_size);
            this.fill = 0;              // how many samples of the current chunk are filled
            this.is_capturing = false;  // push-to-talk state

            this.port.onmessage = (event) =>
            {
                if (event.data === "start")
                {
                    // start from an empty chunk even when the clearing "stop" was lost
                    // (suspend/rebuild) - stale samples must never lead the new talk
                    this.fill = 0;
                    this.is_capturing = true;
                }
                else if (event.data === "stop")
                {
                    this.is_capturing = false;
                    this.fill = 0; // drop any half-filled chunk, next talk starts clean
                }
            };
        }

        process(inputs, outputs)
        {
            // muted: do nothing, output stays silent zeros
            if (this.is_capturing === false)
            {
                return true;
            }

            // inputs[0] is the connected mic chain; it can be absent/empty while disconnected
            let input_channels = inputs[0];
            if (input_channels == null || input_channels.length === 0 || input_channels[0] == null)
            {
                return true;
            }

            let samples = input_channels[0]; // mono capture: first channel only
            // stereo capture: a source with one channel repeats it on the right
            let right_samples = (this.channel_count == 2) ? ((input_channels.length > 1 && input_channels[1] != null) ? input_channels[1] : samples) : null;

            for (let i = 0; i < samples.length; i++)
            {
                this.chunk[this.fill] = samples[i];
                this.fill = this.fill + 1;

                if (right_samples != null)
                {
                    this.chunk[this.fill] = right_samples[i];
                    this.fill = this.fill + 1;
                }

                // chunk full: hand it to the main thread (transfer, no copy) and start a fresh one
                if (this.fill >= this.chunk_size)
                {
                    let full_chunk = this.chunk;
                    this.port.postMessage(full_chunk, [full_chunk.buffer]);
                    this.chunk = new Float32Array(this.chunk_size);
                    this.fill = 0;
                }
            }

            return true; // keep the processor alive
        }
    }

    // makes the processors available under these names for AudioWorkletNode construction
    registerProcessor("lemon-player-processor", LemonPlayerProcessor);
    registerProcessor("lemon-recorder-processor", LemonRecorderProcessor);
}

/**
 * @brief creates the playback output node on the audio context: an AudioWorkletNode when available, the old ScriptProcessorNode otherwise
 *        audioWorklet exists only in secure contexts (https, localhost, 127.0.0.1), so on a plain
 *        http page on a lan ip the fallback is a real, routinely exercised path
 *
 * @return void
 */
function audio__create_audio_player_output()
{
    // start from a clean state; this runs again on every fresh audio_context (reconnect)
    is_audio_worklet_player_active = false;
    audio_player_worklet_node = null;
    is_microphone_worklet_active = false;
    audio_worklet_module_promise = null;

    // capability check: audioWorklet is absent outside secure contexts and in old browsers. deliberately no
    // "is it a file:// page" test: loading the module from disk works in some chromium browsers (edge) and
    // fails in others (chrome, by flags/policy), so the actual failure below decides and falls back then
    if (g_audio_context.audioWorklet == null || typeof AudioWorkletNode == "undefined")
    {
        console.log("audioWorklet unavailable (needs a secure context: https or localhost); using ScriptProcessorNode playback");
        audio__create_script_processor_player();
        return;
    }

    // the worklet must be loaded as a module from a url; stringify the scope function and wrap it
    // in "(...)();" so it executes on load - same trick as the web workers, keeps client.html one file
    let worklet_code_url = URL.createObjectURL(new Blob(['(', audio__lemon_audio_worklet_scope.toString(), ')();'], { type: 'application/javascript' }));

    // the promise is kept: the microphone capture node (built later, at mic permission time) chains on it to
    // use the same module. addModule can throw instead of rejecting (a blocked url fails at call time), and
    // unguarded that exception would end the rest of the audio_enabled handler, the datachannel check included
    try
    {
        audio_worklet_module_promise = g_audio_context.audioWorklet.addModule(worklet_code_url);
    }
    catch (add_module_threw)
    {
        console.log("audioWorklet.addModule threw (" + add_module_threw + "); using ScriptProcessorNode playback");
        audio_worklet_module_promise = null;
        audio__create_script_processor_player();
        return;
    }

    // addModule is async: while it loads, decoded chunks keep landing in audio_queue (see
    // audio__audio_player_write_chunk) and are handed over below once the node is up
    audio_worklet_module_promise.then(function()
    {
        // instantiate the registered processor: no inputs (we feed it via the port), one stereo output
        audio_player_worklet_node = new AudioWorkletNode(g_audio_context, "lemon-player-processor", {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });
        // same output chain as the old player: node -> gain -> speakers
        audio_player_worklet_node.connect(g_audio_player_gain_node);
        is_audio_worklet_player_active = true;
        console.log("playback: AudioWorkletNode active");

        // hand over whatever buffered in audio_queue while the module was loading.
        // copy before transferring: read() returns a view into the queue's backing buffer
        if (audio_queue.length() > 0)
        {
            let buffered_samples = new Float32Array(audio_queue.read(audio_queue.length()));
            audio_player_worklet_node.port.postMessage({ type: "pcm", sender_id: PREMIXED_AUDIO_LANE_ID, pcm: buffered_samples }, [buffered_samples.buffer]);
        }

        // direct message channel: decoder worker -> worklet. one end goes into the worklet, the other into
        // the decoder worker; per-sender pcm then flows between the two threads without passing
        // through the main thread, and the worker stops its 20 ms mixing tick
        let direct_audio_pipe = new MessageChannel();
        audio_player_worklet_node.port.postMessage({ type: "pipe", port: direct_audio_pipe.port2 }, [direct_audio_pipe.port2]);
        g_opus_decoder_worker.postMessage({ type: "use_direct_worklet_pipe", port: direct_audio_pipe.port1 }, [direct_audio_pipe.port1]);

        // the worklet only ever answers a playback_buffer_stats request; the answer is written to the console
        audio_player_worklet_node.port.onmessage = function(event)
        {
            if (event.data != null && event.data.type === "playback_buffer_stats")
            {
                console.log("playback playback_buffers: " + JSON.stringify(event.data.playback_buffers));
            }
        };

        // a bot that was already listed before the node existed gets its large fixed buffer too
        audio__audio_player_announce_music_bots();

        // devtools: __LOG.playback_buffers() prints every playback buffer's depth and target
        if (global.__LOG != null)
        {
            global.__LOG.playback_buffers = audio__audio_player_request_playback_buffer_stats;
        }
    }).catch(function(worklet_setup_error)
    {
        // any setup failure (csp, an unusual browser) falls back to the old path instead of silence
        console.log("AudioWorklet setup failed (" + worklet_setup_error + "); using ScriptProcessorNode playback");
        console.warn("audio playback degraded to main-thread ScriptProcessorNode (higher latency)");
        // null it so the microphone path below takes its "no worklet" branch outright instead
        // of chaining onto a promise that is already rejected
        audio_worklet_module_promise = null;
        audio__create_script_processor_player();
    });
}

/**
 * @brief the legacy playback path: a ScriptProcessorNode calling audio__player_onaudioprocess on the MAIN thread, which drains audio_queue
 *        works everywhere, but stutters when the page is busy
 *
 * @return void
 */
function audio__create_script_processor_player()
{
    // two output channels: the decode/mix pipeline delivers interleaved stereo
    player = g_audio_context.createScriptProcessor(g_audio_config.codec.bufferSize, 1, 2);
    player.onaudioprocess = audio__player_onaudioprocess;
    player.connect(g_audio_player_gain_node);
}

/**
 * @brief the single entry point for decoded pcm: the worklet's pre-mixed buffer when active, audio_queue otherwise (the ScriptProcessor path, and the short window while the worklet module is still loading)
 *
 * @param Float32Array pcm_chunk -> interleaved stereo samples, transferred to the audio thread when the worklet takes them
 *
 * @return void
 */
function audio__audio_player_write_chunk(pcm_chunk)
{
    if (is_audio_worklet_player_active == true)
    {
        // pre-mixed audio goes into the worklet's dedicated low-latency buffer; the buffer is
        // transferred to the audio thread instead of copied
        audio_player_worklet_node.port.postMessage({ type: "pcm", sender_id: PREMIXED_AUDIO_LANE_ID, pcm: pcm_chunk }, [pcm_chunk.buffer]);
    }
    else
    {
        // ScriptProcessor path - and the short window while the worklet module is still loading
        audio_queue.write(pcm_chunk);
    }
}

/**
 * @brief per-user playback volume, applied per sender at the mix inside the player worklet
 *        worklet mode only: the ScriptProcessorNode fallback receives already-mixed audio, so
 *        individual volumes cannot apply there
 *
 * @param number client_id -> the sender
 * @param number volume -> 1.0 unchanged, 0.5 half, 0 silent
 *
 * @return void
 */
function audio__set_client_playback_volume(client_id, volume)
{
    if (is_audio_worklet_player_active == true && audio_player_worklet_node != null)
    {
        audio_player_worklet_node.port.postMessage({ type: "gain", sender_id: client_id, value: volume });
    }
}

/**
 * @brief tells the player worklet which senders are music bots, so their buffers get the large depth the server's lead fills
 *        called whenever the client list changes; the worklet keeps the latest set
 *
 * @return void
 */
function audio__audio_player_announce_music_bots()
{
    if (is_audio_worklet_player_active != true || audio_player_worklet_node == null)
    {
        return;
    }

    let bot_ids = [];
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null && g_client_list[i].is_music_bot == true)
        {
            bot_ids.push(g_client_list[i].client_id);
        }
    }

    audio_player_worklet_node.port.postMessage({ type: "music_bot_senders", ids: bot_ids });
}

/**
 * @brief asks the worklet for a snapshot of every playback buffer (depth and target); the answer is logged
 *
 * @return void
 */
function audio__audio_player_request_playback_buffer_stats()
{
    if (is_audio_worklet_player_active == true && audio_player_worklet_node != null)
    {
        audio_player_worklet_node.port.postMessage({ type: "playback_buffer_stats" });
    }
}

/**
 * @brief flushes all buffered playback, used on a channel switch so old channel audio stops instantly
 *
 * @return void
 */
function audio__audio_player_clear()
{
    audio_queue.clear();

    if (is_audio_worklet_player_active == true)
    {
        audio_player_worklet_node.port.postMessage("clear");
    }
}

/**
 * @brief creates the microphone capture node and wires the capture graph
 *        an AudioWorkletNode (capture runs on the audio thread, immune to the main-thread jank that
 *        used to chop OUTGOING audio) when the worklet module loaded, a ScriptProcessorNode otherwise
 *
 * @return void
 */
function audio__create_microphone_capture_node()
{
    // no worklet support on this context (insecure context / old browser): old path immediately
    if (audio_worklet_module_promise == null)
    {
        audio__create_script_processor_recorder();
        return;
    }

    // the module load was started by audio__create_audio_player_output; chain on it
    audio_worklet_module_promise.then(function()
    {
        // chunk_size 1920 = 40 ms at 48 kHz (two opus frames): captured voice reaches the encoder within 40 ms
        // instead of up to 171 ms in an 8192-sample buffer. the encoder frames internally, so any size is
        // legal; 40 ms balances latency against message rate (25 posts/s; 960 halves the latency at double the traffic)
        g_microphone_recorder = new AudioWorkletNode(g_audio_context, "lemon-recorder-processor", {
            numberOfInputs: 1,
            numberOfOutputs: 1,
            outputChannelCount: [1],
            processorOptions: { chunk_size: 1920 }
        });

        // captured chunks arrive here from the audio thread and go on to the opus encoder
        g_microphone_recorder.port.onmessage = function(event)
        {
            audio__process_captured_microphone_pcm(event.data);
        };

        is_microphone_worklet_active = true;
        console.log("microphone capture: AudioWorkletNode active");
        audio__wire_microphone_capture_graph();
    }).catch(function(recorder_worklet_error)
    {
        console.log("recorder worklet setup failed (" + recorder_worklet_error + "); using ScriptProcessorNode capture");
        audio__create_script_processor_recorder();
    });
}

/**
 * @brief the legacy capture path: a 2048-sample ScriptProcessorNode (~43 ms per callback), then the capture graph
 *        ScriptProcessor buffer sizes must be powers of two, so 960 is not available here
 *
 * @return void
 */
function audio__create_script_processor_recorder()
{
    is_microphone_worklet_active = false;
    // 2048 samples = ~43 ms per callback (was 8192 = 171 ms of built-in send latency);
    // ScriptProcessor buffer sizes must be powers of two, so 960 is not available here
    g_microphone_recorder = g_audio_context.createScriptProcessor(2048, 1, 1);
    audio__wire_microphone_capture_graph();
}

/**
 * @brief mic -> gain -> capture node -> destination
 *        the capture node must reach the destination or the graph never pulls samples through it;
 *        it only ever outputs zeros, so nothing is audible
 *
 * @return void
 */
function audio__wire_microphone_capture_graph()
{
    g_audio_input.connect(g_audio_recorder_gain_node);
    g_audio_recorder_gain_node.connect(g_microphone_recorder);
    g_microphone_recorder.connect(g_audio_context.destination);
}

/**
 * @brief makes a capture node for the sound of a video stream (video.js): stereo, chunks of 20 ms of interleaved samples at the
 *        context's rate, each handed to on_chunk. an AudioWorkletNode of the recorder processor when the worklet module is
 *        available, a ScriptProcessorNode otherwise. the caller connects a source into the node, connects the node to the
 *        destination (the graph pulls samples only along a path that ends there; the node outputs silence) and, on the
 *        worklet node, posts "start" to its port
 *
 * @param function on_chunk -> receives a Float32Array of interleaved left and right samples
 *
 * @return Promise resolves with the node
 */
function audio__create_stream_sound_capture_node(on_chunk)
{
    if (audio_worklet_module_promise == null)
    {
        return Promise.resolve(audio__create_script_processor_stream_sound_capture(on_chunk));
    }

    return audio_worklet_module_promise.then(function()
    {
        // 1920 floats = 960 frames of left and right = 20 ms at 48 kHz, one opus packet of the stream sound.
        // channelCount 2 with "explicit" gives the processor two channels whatever the source has
        let node = new AudioWorkletNode(g_audio_context, "lemon-recorder-processor", {
            numberOfInputs: 1,
            numberOfOutputs: 1,
            outputChannelCount: [1],
            channelCount: 2,
            channelCountMode: "explicit",
            processorOptions: { chunk_size: 1920, channel_count: 2 }
        });

        node.port.onmessage = function(event)
        {
            on_chunk(event.data);
        };

        return node;
    }).catch(function(worklet_error)
    {
        console.log("stream sound capture: the worklet node failed (" + worklet_error + "); using a ScriptProcessorNode");
        return audio__create_script_processor_stream_sound_capture(on_chunk);
    });
}

/**
 * @brief the old capture path for the sound of a video stream: a ScriptProcessorNode of 2048 frames (about 43 ms) with two input channels, interleaved by hand
 *
 * @param function on_chunk -> receives a Float32Array of interleaved left and right samples
 *
 * @return ScriptProcessorNode the node
 */
function audio__create_script_processor_stream_sound_capture(on_chunk)
{
    let node = g_audio_context.createScriptProcessor(2048, 2, 1);

    node.onaudioprocess = function(event)
    {
        let left = event.inputBuffer.getChannelData(0);
        let right = (event.inputBuffer.numberOfChannels > 1) ? event.inputBuffer.getChannelData(1) : left;
        let chunk = new Float32Array(left.length * 2);

        for (let i = 0; i < left.length; i++)
        {
            chunk[2 * i] = left[i];
            chunk[2 * i + 1] = right[i];
        }

        on_chunk(chunk);
    };

    return node;
}

/**
 * @brief the push-to-talk gate for whichever capture node is in use
 *        the worklet is told to start/stop over its port (no pcm crosses threads while muted), the
 *        ScriptProcessorNode gets its callback assigned or removed; the encoder's pending input is
 *        flushed on both edges. safe to call before the capture node exists (does nothing)
 *
 * @param boolean is_active -> true to let pcm flow
 *
 * @return void
 */
function audio__set_microphone_capture_active(is_active)
{
    if (g_microphone_recorder == null)
    {
        return;
    }

    // flush the encoder's pending input on both edges: on stop so nothing of this talk
    // survives, on start in case a stop-side clear was missed (deep idle, toggle paths)
    if (g_opus_encoder_worker != null)
    {
        g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
    }

    if (is_microphone_worklet_active == true)
    {
        g_microphone_recorder.port.postMessage(is_active ? "start" : "stop");
    }
    else
    {
        g_microphone_recorder.onaudioprocess = is_active ? audio__recorder_onaudioprocess : null;
    }
}

/**
 * @brief the ScriptProcessorNode playback callback: fills the output channels from the interleaved queue, zeros on underrun
 *        runs constantly while audio is active, so nothing unnecessary (no html work) belongs in it
 *
 * @param object e -> the AudioProcessingEvent
 *
 * @return void
 */
function audio__player_onaudioprocess(e)
{
    // this function is constantly being run if audio is active
    // do not put any unnseceray code in it, unnsesecary=dealing with html elements
    // same as audio__recorder_onaudioprocess

    let left_channel = e.outputBuffer.getChannelData(0);
    let right_channel = e.outputBuffer.getChannelData(1);

    if (audio_queue.length())
    {
        // the queue holds interleaved stereo (L R L R), two floats per output frame
        let interleaved = audio_queue.read(g_audio_config.codec.bufferSize * 2);
        let frame_count = interleaved.length >> 1;

        for (let i = 0; i < frame_count; i++)
        {
            left_channel[i] = interleaved[i * 2];
            right_channel[i] = interleaved[i * 2 + 1];
        }

        // zero the tail on a short read so no stale samples from the previous callback play
        for (let i = frame_count; i < left_channel.length; i++)
        {
            left_channel[i] = 0;
            right_channel[i] = 0;
        }
    }
    else
    {
        left_channel.set(g_silence);
        right_channel.set(g_silence);
    }
}

/**
 * @brief the ScriptProcessorNode capture fallback, on the MAIN thread; the worklet path delivers the same chunks through the capture node's port instead
 *        both feed audio__process_captured_microphone_pcm
 *
 * @param object event -> the AudioProcessingEvent
 *
 * @return void
 */
function audio__recorder_onaudioprocess(event)
{
    audio__process_captured_microphone_pcm(event.inputBuffer.getChannelData(0));
}

/**
 * @brief one chunk of captured microphone pcm from either capture path: checks the datachannel is usable, then hands the samples to the opus encoder worker
 *
 * @param Float32Array webaudio_captured_bytes -> mono samples
 *
 * @return void
 */
function audio__process_captured_microphone_pcm(webaudio_captured_bytes)
{
    if (audio_transport__ready() === false)
    {
        return;
    }

    if (g_is_microphone_active)
    {
        g_opus_encoder_worker.postMessage({
            type: "encode",
            value: webaudio_captured_bytes
        });
    }
}
