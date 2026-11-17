// owns microphone capture, playback buffers and the Web Audio graph
// audio-opus-glue.js handles encoding and decoding in workers
// worklets handle audio on the rendering thread, with ScriptProcessorNode as a fallback
// each sender has a playback buffer that fills to a target before playing
// underruns increase that target. dropping excess samples limits accumulated delay
// a frame here is two interleaved stereo samples. one rendering quantum is 128 frames

// state private to this file
var player = null;

var audio_player_worklet_node = null;

var is_audio_worklet_player_active = false;

var audio_worklet_module_promise = null; // worklet module loading promise. null when worklets are unavailable

var is_microphone_worklet_active = false;

var PREMIXED_AUDIO_LANE_ID = 9999999; // reserved buffer for mixed audio. matches PREMIXED_LANE_ID in the processor

// reserved sender ID for stream audio. matches STREAM_SOUND_PLAYBACK_BUFFER_ID in the processor
var VIDEO_STREAM_SOUND_SENDER_ID = 9999998;

var audio_queue = {
    buffer: new Float32Array(0),

    // maximum fallback queue depth, about 300 ms of stereo audio
    // discard the oldest samples above this limit to bound playback delay
    max_buffered_samples: 28800,

    /**
     * @brief appends decoded samples and drops the oldest samples above the queue limit
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
 * @brief rebuilds the AudioContext and reconnects playback and any existing microphone
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

    // reinitialize codec workers and the decoder resampler if the device rate changed
    if (g_audio_context.sampleRate !== old_sample_rate)
    {
        g_opus_decoding_sampler = new SpeexResampler(1, 48000, g_audio_context.sampleRate);
        g_opus_decoder_worker.postMessage({ type: "init", sampleRate: g_audio_context.sampleRate });

        if (g_microphone_recorder != null)
        {
            g_opus_encoder_worker.postMessage({ type: "init", sampleRate: g_audio_context.sampleRate });
        }
    }

    // close the old context. it may already be closed
    if (old_context != null)
    {
        try
        {
            old_context.close().catch(function()
            {
            });
        }
        catch (e)
        {
        }
    }

    console.log("audio graph rebuilt on a fresh context, sampleRate " + g_audio_context.sampleRate);
}

/**
 * @brief defines the player and recorder processors loaded on the audio rendering thread
 *
 * @return void
 */
function audio__lemon_audio_worklet_scope()
{
    // AudioWorkletProcessor exists only in the worklet scope. do not call this function on the main thread
    class LemonPlayerProcessor extends AudioWorkletProcessor
    {
        /**
         * @brief initializes sender playback buffers and connects decoder control messages
         */
        constructor()
        {
            super();

            // each sender owns a stereo ring buffer with an adaptive playback target
            // mix the buffers on the audio rendering clock
            this.playback_buffers = new Map();
            this.pending_playback_buffer_gains = new Map(); // volume set before the playback_buffer's first pcm arrived

            // voice buffer tuning. a lower start target reduces delay but tolerates less jitter
            // values count interleaved stereo floats. at 48 kHz, 1 ms is 96 floats
            this.RING_SIZE = 65536;          // interleaved floats per playback_buffer (~680 ms), power of two
            this.RING_MASK = 65535;          // "index & mask" wraps cheaply (replaces % RING_SIZE)
            this.RING_HARD_CAP = 57600;      // about 600 ms. drop the oldest samples above this limit
            this.TARGET_START = 5760;        // initial jitter target: 60 ms of interleaved stereo
            this.TARGET_MIN = 5760;          // never shrink below 60 ms
            this.TARGET_MAX = 28800;         // never grow beyond 300 ms
            this.TARGET_GROW_STEP = 2880;    // +30 ms of cushion per underrun
            this.TARGET_SHRINK_STEP = 960;   // decrease the target by 10 ms after a stable period
            this.SHRINK_AFTER_QUANTA = 1875; // one stable period = ~5 s (375 quanta per second)
            this.SLIP_EXCESS = 1920;         // buffered > target + 20 ms -> slip samples out
            this.FADE_FRAMES = 192;          // fade over 4 ms at playback start and near an empty buffer
            this.PREMIXED_LANE_ID = 9999999; // reserved buffer for mixed audio from the main thread
            this.LANE_IDLE_QUANTA = 3750;    // ~10 s without new data -> playback_buffer deleted
            this.TALK_END_QUANTA = 188;      // about 500 ms without input indicates the talk key was released

            // music bots send audio three seconds ahead of playback
            // keep their larger target aligned with the server MUSIC_BOT_LEAD_MS value
            this.BOT_TARGET = 288000;        // 3 s held before and during playback
            this.BOT_TARGET_MAX = 336000;    // grows to 3.5 s at most after underruns
            this.BOT_RESUME_DEPTH = 96000;   // after a start or an underrun, play again at 1 s of audio
            this.BOT_SLIP_EXCESS = 48000;    // drop excess samples only above the target plus 500 ms
            this.BOT_RING_SIZE = 524288;     // ~5.4 s, power of two
            this.BOT_RING_MASK = 524287;
            this.BOT_RING_HARD_CAP = 432000; // about 4.5 seconds. drop the oldest samples above this limit

            // stream audio needs a deeper buffer to absorb video packet bursts
            // match its initial depth to VIDEO_SOUND_PICTURE_HOLD_MS so pictures and sound wait equally
            this.STREAM_SOUND_PLAYBACK_BUFFER_ID = 9999998;
            this.STREAM_SOUND_TARGET = 28800;        // 300 ms held before playback starts, and again after an underrun
            this.STREAM_SOUND_TARGET_MAX = 57600;    // grows to 600 ms at most after underruns
            this.STREAM_SOUND_SLIP_EXCESS = 28800;   // slip only above target + 300 ms
            this.STREAM_SOUND_RING_HARD_CAP = 96000; // about one second. drop the oldest samples above this limit
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

                // receive PCM directly from the decoder worker without routing it through the page
                if (event.data.type === "pipe")
                {
                    event.data.port.onmessage = (pipe_event) =>
                    {
                        if (pipe_event.data.type === "clear_sender")
                        {
                            this.playback_buffers.delete(pipe_event.data.sender_id);
                            return;
                        }
                        this.playback_buffer_write(pipe_event.data.id, pipe_event.data.pcm);
                    };
                    return;
                }

                // apply each sender volume during mixing
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

                // audio buffered during module loading uses its own small playback buffer
                if (event.data.type === "pcm")
                {
                    this.playback_buffer_write(event.data.sender_id, event.data.pcm);
                }
            };
        }

        // appends one pcm chunk to a sender's buffer, creating the buffer on first use
        /**
         * @brief queues interleaved stereo samples using the sender's voice, bot or stream buffering limits
         *
         * @param number sender_id -> sender or reserved playback buffer ID
         * @param Float32Array chunk -> interleaved stereo PCM samples
         *
         * @return void
         */
        playback_buffer_write(sender_id, chunk)
        {
            let playback_buffer = this.playback_buffers.get(sender_id);

            if (playback_buffer == null)
            {
                let is_music_bot = this.music_bot_senders.has(sender_id);
                let is_premixed = (sender_id === this.PREMIXED_LANE_ID);
                let is_stream_sound = (sender_id === this.STREAM_SOUND_PLAYBACK_BUFFER_ID);

                // buffer kinds: mixed startup audio, adaptive voice, music bots and video stream sound
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

            // drop the oldest samples at the hard limit to bound playback delay
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

        // mix one rendering quantum into the zeroed output, then limit its amplitude
        /**
         * @brief mixes ready sender buffers into the output and adjusts buffering after underruns
         *
         * @param array inputs -> unused worklet input buffers
         * @param array outputs -> output buses containing left and optional right sample arrays
         *
         * @return boolean -> true to keep the playback processor running
         */
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

                // release buffered transmission tails when a sender has stopped
                if (playback_buffer.state === 0 && this.quantum_counter - playback_buffer.last_data_quantum > this.TALK_END_QUANTA)
                {
                    playback_buffer.available = 0;
                    playback_buffer.read_index = playback_buffer.write_index;
                    playback_buffer.target = playback_buffer.target_min;
                    playback_buffer.stable_quanta = 0;
                }

                // start playback with a fade after reaching the buffer target
                // bots can start with one second buffered while the server fills the remaining lead
                if (playback_buffer.state === 0)
                {
                    if (playback_buffer.available < (playback_buffer.is_music_bot ? this.BOT_RESUME_DEPTH : playback_buffer.target))
                    {
                        continue;
                    }
                    playback_buffer.state = 1;
                    playback_buffer.fade_in_remaining = this.FADE_FRAMES;
                }

                // shrink voice targets after stable playback. bot targets remain fixed
                playback_buffer.stable_quanta = playback_buffer.stable_quanta + 1;
                if (playback_buffer.stable_quanta >= this.SHRINK_AFTER_QUANTA)
                {
                    playback_buffer.stable_quanta = 0;
                    if (playback_buffer.is_music_bot == false && playback_buffer.target > playback_buffer.target_min)
                    {
                        playback_buffer.target = playback_buffer.target - this.TARGET_SHRINK_STEP;
                    }
                }

                // drop one excess frame per quantum to reduce accumulated delay gradually
                if (playback_buffer.available > playback_buffer.target + playback_buffer.slip_excess)
                {
                    playback_buffer.read_index = (playback_buffer.read_index + 2) & playback_buffer.ring_mask;
                    playback_buffer.available = playback_buffer.available - 2;
                }

                for (let i = 0; i < frame_count; i++)
                {
                    if (playback_buffer.available < 2)
                    {
                        // after an underrun, collect a larger buffer before resuming
                        playback_buffer.state = 0;
                        playback_buffer.stable_quanta = 0;
                        if (playback_buffer.target < playback_buffer.target_max)
                        {
                            playback_buffer.target = playback_buffer.target + this.TARGET_GROW_STEP;
                        }
                        break;
                    }

                    let gain = playback_buffer.gain;

                    // fade in after playback starts
                    if (playback_buffer.fade_in_remaining > 0)
                    {
                        gain = gain * (1 - playback_buffer.fade_in_remaining / this.FADE_FRAMES);
                        playback_buffer.fade_in_remaining = playback_buffer.fade_in_remaining - 1;
                    }

                    // fade out before the buffer becomes empty
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

            // pass amplitudes up to 0.85 unchanged and limit louder sums smoothly
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

    // collect capture samples into chunks for the encoder worker
    // port messages start and stop transmission
    class LemonRecorderProcessor extends AudioWorkletProcessor
    {
        /**
         * @brief initializes capture chunks and binds microphone start and stop controls
         *
         * @param object options -> processor options for chunk size and channel count
         */
        constructor(options)
        {
            super();
            // chunk size comes from the main thread (g_audio_config.codec.bufferSize) so the opus
            // encoder worker sees identical input on both capture paths
            this.chunk_size = (options && options.processorOptions && options.processorOptions.chunk_size) ? options.processorOptions.chunk_size : 8192;
            // capture one microphone channel or interleaved stereo stream audio
            // stereo chunk sizes must be even
            this.channel_count = (options && options.processorOptions && options.processorOptions.channel_count == 2) ? 2 : 1;
            this.chunk = new Float32Array(this.chunk_size);
            this.fill = 0;              // how many samples of the current chunk are filled
            this.is_capturing = false;  // talk control state

            this.port.onmessage = (event) =>
            {
                if (event.data === "start")
                {
                    // start with an empty chunk even if a previous stop was missed during suspension
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

        /**
         * @brief collects active microphone or stream samples into transferable chunks
         *
         * @param array inputs -> input buses containing channel sample arrays
         * @param array outputs -> unused output buses kept silent
         *
         * @return boolean -> true to keep the capture processor running
         */
        process(inputs, outputs)
        {
            // muted: do nothing, output stays silent zeros
            if (this.is_capturing === false)
            {
                return true;
            }

            // the microphone input can be absent while disconnected
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
 * @brief creates worklet playback when available or uses the script processor fallback
 *
 * @return void
 */
function audio__create_audio_player_output()
{
    // clear playback state when initializing a new AudioContext
    is_audio_worklet_player_active = false;
    audio_player_worklet_node = null;
    is_microphone_worklet_active = false;
    audio_worklet_module_promise = null;

    // try worklets when available. fall back if loading fails in this browser or context
    if (g_audio_context.audioWorklet == null || typeof AudioWorkletNode == "undefined")
    {
        console.log("audioWorklet unavailable (needs a secure context: https or localhost); using ScriptProcessorNode playback");
        audio__create_script_processor_player();
        return;
    }

    // load the serialized scope function as a module so the client remains one file
    let worklet_code = '(' + audio__lemon_audio_worklet_scope.toString() + ')();';

    // choose module address
    let is_page_opened_from_file = (location.protocol === "file:");
    let worklet_code_url = is_page_opened_from_file
        ? "data:application/javascript;charset=utf-8," + encodeURIComponent(worklet_code)
        : URL.createObjectURL(new Blob([worklet_code], { type: 'application/javascript' }));

    // keep the module promise for microphone setup
    // handle both a synchronous addModule exception and a rejected promise
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

    // decoded chunks wait in audio_queue until the worklet is ready
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

        // hand over whatever buffered in audio_queue while the module was loading
        // copy before transferring: read() returns a view into the queue's backing buffer
        if (audio_queue.length() > 0)
        {
            let buffered_samples = new Float32Array(audio_queue.read(audio_queue.length()));
            audio_player_worklet_node.port.postMessage({ type: "pcm", sender_id: PREMIXED_AUDIO_LANE_ID, pcm: buffered_samples }, [buffered_samples.buffer]);
        }

        // connect decoder and worklet ports directly, then stop the fallback mixing timer
        let direct_audio_pipe = new MessageChannel();
        audio_player_worklet_node.port.postMessage({ type: "pipe", port: direct_audio_pipe.port2 }, [direct_audio_pipe.port2]);
        g_opus_decoder_worker.postMessage({ type: "use_direct_worklet_pipe", port: direct_audio_pipe.port1 }, [direct_audio_pipe.port1]);

        // log replies to playback_buffer_stats requests
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
 * @brief creates fallback playback on the main thread through ScriptProcessorNode
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
 * @brief sends decoded PCM to the worklet or queues it for fallback playback
 *
 * @param Float32Array pcm_chunk -> interleaved stereo samples, transferred to the audio thread when the worklet takes them
 *
 * @return void
 */
function audio__audio_player_write_chunk(pcm_chunk)
{
    if (is_audio_worklet_player_active == true)
    {
        // transfer mixed PCM to the worklet buffer without copying it
        audio_player_worklet_node.port.postMessage({ type: "pcm", sender_id: PREMIXED_AUDIO_LANE_ID, pcm: pcm_chunk }, [pcm_chunk.buffer]);
    }
    else
    {
        // queue audio during worklet loading or fallback playback
        audio_queue.write(pcm_chunk);
    }
}

/**
 * @brief sets one sender volume in worklet playback
 * fallback playback is already mixed, so it cannot adjust individual senders
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
 * @brief updates the worklet with the current music bot senders
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
 * @brief asks the worklet for a snapshot of every playback buffer (depth and target). the answer is logged
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
 * @brief creates worklet microphone capture or falls back to ScriptProcessorNode
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
        // use 1920 samples, or 40 ms at 48 kHz, to balance capture delay and message rate
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
 * @brief creates a 2048 sample ScriptProcessorNode microphone capture
 *
 * @return void
 */
function audio__create_script_processor_recorder()
{
    is_microphone_worklet_active = false;
    // 2048 samples take about 43 ms at 48 kHz. ScriptProcessor buffer sizes must be powers of two
    g_microphone_recorder = g_audio_context.createScriptProcessor(2048, 1, 1);
    audio__wire_microphone_capture_graph();
}

/**
 * @brief connects microphone, gain and capture to the audio destination
 * the silent capture output must remain connected so the graph keeps pulling input
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
 * @brief creates stereo capture that passes stream audio chunks to the supplied callback
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
        // 1920 floats = 960 frames of left and right = 20 ms at 48 kHz, one opus packet of the stream sound
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
 * @brief creates stereo ScriptProcessorNode capture and forwards interleaved samples
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
 * @brief starts or stops microphone capture and flushes pending encoder input
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

    // clear encoder input on both start and stop so partial samples cannot cross transmissions
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
 * @brief fills fallback playback channels from the queue and pads underruns with silence
 *
 * @param object e -> the AudioProcessingEvent
 *
 * @return void
 */
function audio__player_onaudioprocess(e)
{
    // keep this audio callback free of DOM work

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
 * @brief forwards fallback microphone samples to the shared capture handler
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
 * @brief sends active microphone samples to the encoder while an audio transport is ready
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
