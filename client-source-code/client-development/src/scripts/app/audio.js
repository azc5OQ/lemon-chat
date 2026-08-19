// ===========================================================================
//  START HERE. WHAT THE AUDIO CODE IS, IN PLAIN WORDS.
// ---------------------------------------------------------------------------
//  WHICH FILE DOES WHAT
//
//  audio.js (this file)  Moves sound around. Microphone in, speakers out,
//                        buffering and timing. It never compresses anything.
//
//  audio-opus-glue.js    Turns sound into small network packets and back, with
//                        the opus codec. It runs inside web workers.
//
//  Between the two files, sound is plain numbers (Float32Array). On the
//  network, sound is opus packets. This file only ever sees plain numbers.
//
//  Two blocks of numbers are safe to tune. "AUDIO WORKLET TUNABLES", further
//  down in this file, sets delay against smoothness. "OPUS AUDIO TUNABLES", at
//  the top of audio-opus-glue.js, sets compression.
// ---------------------------------------------------------------------------
//  WORDS WE INVENTED, OR USE IN OUR OWN WAY
//
//  lane        One speaker's private playback buffer. Three people talking
//              means three lanes, and we add them together at the very end.
//
//  premixed lane   A fake lane, id 9999999, for sound that something else
//                  already mixed. It is not a person.
//
//  target      How much sound a lane collects before it starts to play. This
//              cushion hides network delay. Bigger is smoother but later.
//
//  prebuffering    A lane that is collecting sound and not playing yet,
//                  because it has not reached its target.
//
//  underrun    A playing lane ran out of sound. Normally the network was late,
//              so we grow that lane's target to make it happen less often.
//
//  slipping    Quietly deleting one frame when a lane holds far too much. It
//              removes delay a little at a time and nobody hears it.
//
//  quantum     One tick of the audio engine: 128 frames, about 2.7 ms. There
//              are about 375 ticks in a second.
//
//  interleaved     Stereo stored as one list: left, right, left, right. So the
//                  count of numbers is always twice the count of frames.
//
//  frame       One moment of stereo sound, so two numbers.
//              CAREFUL: audio-opus-glue.js says "frame" for a different thing,
//              one opus packet, which is 40 ms of sound.
//
//  worklet     The modern way to run audio code on its own thread. Preferred.
//  ScriptProcessor     The old and slower way. Used only if worklet is missing.
//
//  capture graph   Microphone -> gain -> recorder node. The way in.
//  playback graph  Player node -> gain -> speakers. The way out.
// ===========================================================================
// contents: the worklet processors (player + recorder), their ScriptProcessorNode
// fallbacks, playback and capture graph construction, the push-to-talk capture gate,
// and the after-idle context rebuild. UI glue (buttons, key handlers) stays in main.js

// builds a brand new AudioContext and re-wires playback and (if present) the microphone
// onto it - resuming a context whose HAL android reset is what corrupted both directions
function rebuild_audio_graph_after_idle()
{
    let old_context = audio_context;
    let old_sample_rate = (old_context != null) ? old_context.sampleRate : 48000;

    try
    {
        audio_context = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
    }
    catch (sample_rate_hint_not_supported)
    {
        audio_context = new (window.AudioContext || window.webkitAudioContext)();
    }

    // playback graph: gain -> speakers, then the player node (worklet or script processor)
    g_audio_player_gain_node = audio_context.createGain();
    g_audio_player_gain_node.connect(audio_context.destination);
    create_audio_player_output();

    // microphone: the MediaStream survives a context swap, the nodes do not
    if (g_local_audio_stream != null && g_microphone_recorder != null)
    {
        // create_microphone_capture_node builds a fresh capture node on the new context
        g_microphone_recorder = null;
        g_audio_input = audio_context.createMediaStreamSource(g_local_audio_stream);
        g_audio_recorder_gain_node = audio_context.createGain();
        create_microphone_capture_node();
    }

    // a fresh context may come up at a different device rate; the decode resampler and
    // both codec workers are pinned to it, so re-init them on a change
    if (audio_context.sampleRate !== old_sample_rate)
    {
        g_opus_decoding_sampler = new SpeexResampler(1, 48000, audio_context.sampleRate);
        g_opus_decoder_worker.postMessage({ type: "init", sampleRate: audio_context.sampleRate });

        if (g_microphone_recorder != null)
        {
            g_opus_encoder_worker.postMessage({ type: "init", sampleRate: audio_context.sampleRate });
        }
    }

    // the old context is dead weight; close() may reject on an already-dead one, both fine
    if (old_context != null)
    {
        try { old_context.close().catch(function() {}); } catch (e) {}
    }

    console.log("audio graph rebuilt on a fresh context, sampleRate " + audio_context.sampleRate);
}

// AudioWorklet playback processor. this function is never called on the main thread: its source
// is stringified into a blob module and runs on the browser's dedicated audio rendering thread,
// where main-thread jank (busy UI, big DOM updates) cannot starve it - the ScriptProcessorNode
// it replaces ran its callback on the main thread and stuttered whenever the page was busy.
// it keeps its own ring buffer of interleaved stereo samples: chunks arrive over the port,
// oldest samples are dropped beyond ~300 ms of backlog (same policy as g_audio_queue), and each
// 128-frame render quantum is filled from the ring (zeros on underrun)
function lemon_audio_worklet_scope()
{
    // AudioWorkletProcessor is a class the browser only defines inside the audio worklet scope,
    // never on the main thread - another reason this function must not be called directly
    class LemonPlayerProcessor extends AudioWorkletProcessor
    {
        constructor()
        {
            super();

            // per-SENDER playback lanes: sender id -> its own jitter-buffered ring of interleaved
            // stereo. each lane prebuffers to a target depth before it plays, grows the target
            // when it underruns (jittery network needs more cushion) and shrinks it back plus
            // slips out excess samples once the flow proves stable. lanes are mixed here, on the
            // audio clock - the g_decoder worker pipes raw per-sender pcm via a MessagePort
            this.lanes = new Map();
            this.pending_lane_gains = new Map(); // volume set before the lane's first pcm arrived

            // ================================================================
            //  AUDIO WORKLET TUNABLES  -  the per-sender jitter buffer.
            // ----------------------------------------------------------------
            //  THIS is what sets felt playback latency vs. smoothness on the
            //  normal (worklet) path. TARGET_START is the delay each voice
            //  adds before it starts playing - lower it for less latency,
            //  raise it if choppy on bad networks. All values are in
            //  interleaved-stereo floats: 48000 Hz x 2 ch = 96000 floats/sec,
            //  so 1 ms = 96 floats and 60 ms = 5760 floats.
            //  (The opus decode/PLC knobs are in audio-opus-glue.js under
            //   "OPUS AUDIO TUNABLES".)
            // ==============================================================
            this.RING_SIZE = 65536;          // interleaved floats per lane (~680 ms), power of two
            this.RING_MASK = 65535;          // "index & mask" wraps cheaply (replaces % RING_SIZE)
            this.RING_HARD_CAP = 57600;      // ~600 ms; beyond this the oldest samples are dropped
            this.TARGET_START = 5760;        // initial jitter target: 60 ms of interleaved stereo
            this.TARGET_MIN = 5760;          // never shrink below 60 ms
            this.TARGET_MAX = 28800;         // never grow beyond 300 ms
            this.TARGET_GROW_STEP = 2880;    // +30 ms of cushion per underrun
            this.TARGET_SHRINK_STEP = 960;   // -10 ms per stable period
            this.SHRINK_AFTER_QUANTA = 1875; // one stable period = ~5 s (375 quanta per second)
            this.SLIP_EXCESS = 1920;         // buffered > target + 20 ms -> slip samples out
            this.FADE_FRAMES = 192;          // 4 ms fade at lane (re)start and near-empty edges
            this.PREMIXED_LANE_ID = 9999999; // lane for already-mixed audio from the main thread
            this.LANE_IDLE_QUANTA = 3750;    // ~10 s without new data -> lane deleted
            this.TALK_END_QUANTA = 188;      // ~500 ms without new data -> push-to-talk was released

            this.quantum_counter = 0;

            this.port.onmessage = (event) =>
            {
                // "clear" = channel switch: throw away everything buffered in every lane
                if (event.data === "clear")
                {
                    this.lanes.clear();
                    return;
                }

                // the g_decoder worker's end of the direct pipe: per-sender pcm arrives here
                // from the worker thread without ever touching the main thread
                if (event.data.type === "pipe")
                {
                    event.data.port.onmessage = (pipe_event) =>
                    {
                        this.lane_write(pipe_event.data.id, pipe_event.data.pcm);
                    };
                    return;
                }

                // per-user playback volume, applied at the mix below
                if (event.data.type === "gain")
                {
                    let lane = this.lanes.get(event.data.sender_id);
                    if (lane != null)
                    {
                        lane.gain = event.data.value;
                    }
                    // remember it for a lane that does not exist yet (user preset before talking)
                    this.pending_lane_gains.set(event.data.sender_id, event.data.value);
                    return;
                }

                // pre-mixed pcm from the main thread (the flush of audio buffered while this
                // module was still loading); plays through its own low-latency lane
                if (event.data.type === "pcm")
                {
                    this.lane_write(event.data.sender_id, event.data.pcm);
                }
            };
        }

        // appends one pcm chunk to a sender's lane, creating the lane on first use
        lane_write(sender_id, chunk)
        {
            let lane = this.lanes.get(sender_id);

            if (lane == null)
            {
                lane = {
                    ring: new Float32Array(this.RING_SIZE),
                    read_index: 0,
                    write_index: 0,
                    available: 0,
                    state: 0, // 0 = prebuffering, 1 = playing
                    target: sender_id === this.PREMIXED_LANE_ID ? 960 : this.TARGET_START,
                    fade_in_remaining: 0,
                    stable_quanta: 0,
                    last_data_quantum: this.quantum_counter,
                    gain: this.pending_lane_gains.has(sender_id) ? this.pending_lane_gains.get(sender_id) : 1.0
                };
                this.lanes.set(sender_id, lane);
            }

            lane.last_data_quantum = this.quantum_counter;

            // hard cap: drop the oldest first - delay that piled up is never recovered by itself
            if (lane.available + chunk.length > this.RING_HARD_CAP)
            {
                let drop = lane.available + chunk.length - this.RING_HARD_CAP;
                if (drop > lane.available)
                {
                    drop = lane.available;
                }
                lane.read_index = (lane.read_index + drop) & this.RING_MASK;
                lane.available = lane.available - drop;
            }

            for (let i = 0; i < chunk.length; i++)
            {
                lane.ring[lane.write_index] = chunk[i];
                lane.write_index = (lane.write_index + 1) & this.RING_MASK;
            }
            lane.available = lane.available + chunk.length;
        }

        // called by the audio engine every render quantum (128 frames, ~2.7 ms at 48 kHz);
        // output buffers arrive zeroed, lanes are summed into them, then soft-limited
        process(inputs, outputs)
        {
            let left_channel = outputs[0][0];
            let right_channel = outputs[0].length > 1 ? outputs[0][1] : null;
            let frame_count = left_channel.length;

            this.quantum_counter = this.quantum_counter + 1;

            for (const [sender_id, lane] of this.lanes)
            {
                // sender went silent long ago: free the lane (next talk starts a fresh one)
                if (this.quantum_counter - lane.last_data_quantum > this.LANE_IDLE_QUANTA)
                {
                    this.lanes.delete(sender_id);
                    continue;
                }

                // a lane still prebuffering with no new audio for a while: the key was released, so
                // what sits in the ring is last transmission's tail and the grown cushion is stale
                if (lane.state === 0 && this.quantum_counter - lane.last_data_quantum > this.TALK_END_QUANTA)
                {
                    lane.available = 0;
                    lane.read_index = lane.write_index;
                    lane.target = sender_id === this.PREMIXED_LANE_ID ? 960 : this.TARGET_MIN;
                    lane.stable_quanta = 0;
                }

                // prebuffering: hold until the jitter target is reached, then start with a fade-in
                if (lane.state === 0)
                {
                    if (lane.available < lane.target)
                    {
                        continue;
                    }
                    lane.state = 1;
                    lane.fade_in_remaining = this.FADE_FRAMES;
                }

                // stable long enough: shrink the target one step back toward the minimum
                lane.stable_quanta = lane.stable_quanta + 1;
                if (lane.stable_quanta >= this.SHRINK_AFTER_QUANTA)
                {
                    lane.stable_quanta = 0;
                    if (lane.target > this.TARGET_MIN)
                    {
                        lane.target = lane.target - this.TARGET_SHRINK_STEP;
                    }
                }

                // sample slipping: while buffered depth exceeds target + 20 ms, silently drop one
                // frame per quantum (~0.8% time compression) - drains built-up delay with no click
                if (lane.available > lane.target + this.SLIP_EXCESS)
                {
                    lane.read_index = (lane.read_index + 2) & this.RING_MASK;
                    lane.available = lane.available - 2;
                }

                for (let i = 0; i < frame_count; i++)
                {
                    if (lane.available < 2)
                    {
                        // underrun: back to prebuffering with a bigger cushion; the fade-out
                        // below already brought the level to ~zero, so no click
                        lane.state = 0;
                        lane.stable_quanta = 0;
                        if (lane.target < this.TARGET_MAX)
                        {
                            lane.target = lane.target + this.TARGET_GROW_STEP;
                        }
                        break;
                    }

                    let gain = lane.gain;

                    // fade-in over the first few ms after a (re)start
                    if (lane.fade_in_remaining > 0)
                    {
                        gain = gain * (1 - lane.fade_in_remaining / this.FADE_FRAMES);
                        lane.fade_in_remaining = lane.fade_in_remaining - 1;
                    }

                    // fade-out toward an approaching empty buffer, so starvation never clicks
                    let frames_left = lane.available >> 1;
                    if (frames_left < this.FADE_FRAMES)
                    {
                        gain = gain * (frames_left / this.FADE_FRAMES);
                    }

                    // one output frame consumes two ring samples: left then right (interleaved)
                    left_channel[i] = left_channel[i] + lane.ring[lane.read_index] * gain;
                    lane.read_index = (lane.read_index + 1) & this.RING_MASK;

                    let right_sample = lane.ring[lane.read_index];
                    lane.read_index = (lane.read_index + 1) & this.RING_MASK;
                    lane.available = lane.available - 2;

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

    // microphone capture processor: the mirror image of the player. the mic chain is connected
    // INTO this node; every 128-frame quantum it appends the captured samples to a chunk of the
    // same size the old ScriptProcessorNode delivered, and posts full chunks to the main thread,
    // which forwards them to the opus g_encoder worker. push-to-talk gates it via "start"/"stop"
    // port messages - while stopped, no pcm leaves the audio thread at all
    class LemonRecorderProcessor extends AudioWorkletProcessor
    {
        constructor(options)
        {
            super();
            // chunk size comes from the main thread (g_audio_config.codec.bufferSize) so the opus
            // g_encoder worker sees identical input on both capture paths
            this.chunk_size = (options && options.processorOptions && options.processorOptions.chunk_size) ? options.processorOptions.chunk_size : 8192;
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

            for (let i = 0; i < samples.length; i++)
            {
                this.chunk[this.fill] = samples[i];
                this.fill = this.fill + 1;

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

// creates the playback output node on audio_context: AudioWorkletNode when available, the old
// ScriptProcessorNode otherwise. audioWorklet exists only in SECURE contexts (https, localhost,
// 127.0.0.1) - a plain http page on a LAN ip does not have it - so the fallback is a real,
// routinely exercised path, not an afterthought
function create_audio_player_output()
{
    // start from a clean state; this runs again on every fresh audio_context (reconnect)
    g_is_audio_worklet_player_active = false;
    g_audio_player_worklet_node = null;
    g_is_microphone_worklet_active = false;
    g_audio_worklet_module_promise = null;

    // capability check: audioWorklet is simply absent outside secure contexts and in old browsers.
    // NOTE: deliberately no "is it a file:// page" test here. loading the worklet module from a
    // page opened straight from disk works in some chromium browsers (edge) and fails in others
    // (chrome, depending on flags/policy), so this detects the ACTUAL failure below and falls
    // back then - a blanket file:// rule would downgrade the browsers where it works fine
    if (audio_context.audioWorklet == null || typeof AudioWorkletNode == "undefined")
    {
        console.log("audioWorklet unavailable (needs a secure context: https or localhost); using ScriptProcessorNode playback");
        create_script_processor_player();
        return;
    }

    // the worklet must be loaded as a module from a url; stringify the scope function and wrap it
    // in "(...)();" so it executes on load - same trick as the web workers, keeps client.html one file
    let worklet_code_url = URL.createObjectURL(new Blob(['(', lemon_audio_worklet_scope.toString(), ')();'], { type: 'application/javascript' }));

    // the promise is kept: the microphone capture node creation (which happens later, at mic
    // permission time) chains on it to build its AudioWorkletNode from the same module.
    // addModule can THROW instead of returning a rejected promise (a blocked url is rejected
    // at call time): unguarded, that exception escapes this whole function and kills the rest
    // of the audio_enabled handler - including the webrtc datachannel check that follows it
    try
    {
        g_audio_worklet_module_promise = audio_context.audioWorklet.addModule(worklet_code_url);
    }
    catch (add_module_threw)
    {
        console.log("audioWorklet.addModule threw (" + add_module_threw + "); using ScriptProcessorNode playback");
        g_audio_worklet_module_promise = null;
        create_script_processor_player();
        return;
    }

    // addModule is async: while it loads, decoded chunks keep landing in g_audio_queue (see
    // audio_player_write_chunk) and are handed over below once the node is up
    g_audio_worklet_module_promise.then(function()
    {
        // instantiate the registered processor: no inputs (we feed it via the port), one stereo output
        g_audio_player_worklet_node = new AudioWorkletNode(audio_context, "lemon-player-processor", {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });
        // same output chain as the old player: node -> gain -> speakers
        g_audio_player_worklet_node.connect(g_audio_player_gain_node);
        g_is_audio_worklet_player_active = true;
        console.log("playback: AudioWorkletNode active");

        // hand over whatever buffered in g_audio_queue while the module was loading.
        // copy before transferring: read() returns a view into the queue's backing buffer
        if (g_audio_queue.length() > 0)
        {
            let buffered_samples = new Float32Array(g_audio_queue.read(g_audio_queue.length()));
            g_audio_player_worklet_node.port.postMessage({ type: "pcm", sender_id: g_PREMIXED_AUDIO_LANE_ID, pcm: buffered_samples }, [buffered_samples.buffer]);
        }

        // direct pipe: g_decoder worker -> worklet. one end goes into the worklet, the other into
        // the g_decoder worker; per-sender pcm then flows between the two threads with no
        // main-thread hop, and the worker stops its 20 ms mixing tick
        let direct_audio_pipe = new MessageChannel();
        g_audio_player_worklet_node.port.postMessage({ type: "pipe", port: direct_audio_pipe.port2 }, [direct_audio_pipe.port2]);
        g_opus_decoder_worker.postMessage({ type: "use_direct_worklet_pipe", port: direct_audio_pipe.port1 }, [direct_audio_pipe.port1]);
    }).catch(function(worklet_setup_error)
    {
        // any setup failure (csp, quirky browser) lands on the old path instead of g_silence
        console.log("AudioWorklet setup failed (" + worklet_setup_error + "); using ScriptProcessorNode playback");
        console.warn("audio playback degraded to main-thread ScriptProcessorNode (higher latency)");
        // null it so the microphone path below takes its "no worklet" branch outright instead
        // of chaining onto a promise that is already rejected
        g_audio_worklet_module_promise = null;
        create_script_processor_player();
    });
}

// the legacy playback path: ScriptProcessorNode calls player_onaudioprocess on the MAIN thread,
// which drains g_audio_queue - works everywhere, but stutters when the page is busy
function create_script_processor_player()
{
    // two output channels: the decode/mix pipeline delivers interleaved stereo
    player = audio_context.createScriptProcessor(g_audio_config.codec.bufferSize, 1, 2);
    player.onaudioprocess = player_onaudioprocess;
    player.connect(g_audio_player_gain_node);
}

// single entry point for decoded PCM: worklet ring when active, g_audio_queue (ScriptProcessor
// path, and the short window while the worklet module is still loading) otherwise
function audio_player_write_chunk(pcm_chunk)
{
    if (g_is_audio_worklet_player_active == true)
    {
        // pre-mixed audio goes into the worklet's dedicated low-latency lane; the buffer is
        // transferred to the audio thread instead of copied
        g_audio_player_worklet_node.port.postMessage({ type: "pcm", sender_id: g_PREMIXED_AUDIO_LANE_ID, pcm: pcm_chunk }, [pcm_chunk.buffer]);
    }
    else
    {
        // ScriptProcessor path - and the short window while the worklet module is still loading
        g_audio_queue.write(pcm_chunk);
    }
}

// per-user playback volume (1.0 = unchanged, 0.5 = half, 0 = silent), applied per sender at the
// mix inside the player worklet. worklet mode only: the ScriptProcessorNode fallback receives
// already-mixed audio, so individual volumes cannot apply there
function set_client_playback_volume(client_id, volume)
{
    if (g_is_audio_worklet_player_active == true && g_audio_player_worklet_node != null)
    {
        g_audio_player_worklet_node.port.postMessage({ type: "gain", sender_id: client_id, value: volume });
    }
}

// flushes all buffered playback (used on channel switch, so old channel audio stops instantly)
function audio_player_clear()
{
    g_audio_queue.clear();

    if (g_is_audio_worklet_player_active == true)
    {
        g_audio_player_worklet_node.port.postMessage("clear");
    }
}

// creates the microphone capture node: AudioWorkletNode (capture runs on the audio thread,
// immune to main-thread jank that used to chop OUTGOING audio) when the worklet module loaded,
// ScriptProcessorNode otherwise. wires the capture graph either way
function create_microphone_capture_node()
{
    // no worklet support on this context (insecure context / old browser): old path immediately
    if (g_audio_worklet_module_promise == null)
    {
        create_script_processor_recorder();
        return;
    }

    // the module load was started by create_audio_player_output; chain on it
    g_audio_worklet_module_promise.then(function()
    {
        // chunk_size 1920 = 40 ms at 48 kHz (two opus frames): captured voice reaches the g_encoder
        // within 40 ms instead of sitting up to 171 ms in an 8192-sample buffer. the g_encoder
        // frames internally, so any chunk size is legal - 40 ms balances latency against
        // message rate (25 posts/s; 960 would halve the latency at double the traffic)
        g_microphone_recorder = new AudioWorkletNode(audio_context, "lemon-recorder-processor", {
            numberOfInputs: 1,
            numberOfOutputs: 1,
            outputChannelCount: [1],
            processorOptions: { chunk_size: 1920 }
        });

        // captured chunks arrive here from the audio thread and go on to the opus g_encoder
        g_microphone_recorder.port.onmessage = function(event)
        {
            process_captured_microphone_pcm(event.data);
        };

        g_is_microphone_worklet_active = true;
        console.log("microphone capture: AudioWorkletNode active");
        wire_microphone_capture_graph();
    }).catch(function(recorder_worklet_error)
    {
        console.log("recorder worklet setup failed (" + recorder_worklet_error + "); using ScriptProcessorNode capture");
        create_script_processor_recorder();
    });
}

function create_script_processor_recorder()
{
    g_is_microphone_worklet_active = false;
    // 2048 samples = ~43 ms per callback (was 8192 = 171 ms of built-in send latency);
    // ScriptProcessor buffer sizes must be powers of two, so 960 is not available here
    g_microphone_recorder = audio_context.createScriptProcessor(2048, 1, 1);
    wire_microphone_capture_graph();
}

// mic -> gain -> capture node -> destination. the capture node must reach the destination or
// the graph never pulls samples through it; it only ever outputs zeros, so nothing is audible
function wire_microphone_capture_graph()
{
    g_audio_input.connect(g_audio_recorder_gain_node);
    g_audio_recorder_gain_node.connect(g_microphone_recorder);
    g_microphone_recorder.connect(audio_context.destination);
}

// push-to-talk gate for whichever capture node is in use: the worklet is told to start/stop
// over its port (no pcm crosses threads while muted), the ScriptProcessorNode gets its callback
// assigned/removed like before. safe to call before the capture node exists (does nothing)
function set_microphone_capture_active(is_active)
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

    if (g_is_microphone_worklet_active == true)
    {
        g_microphone_recorder.port.postMessage(is_active ? "start" : "stop");
    }
    else
    {
        g_microphone_recorder.onaudioprocess = is_active ? recorder_onaudioprocess : null;
    }
}

function player_onaudioprocess(e)
{
    // this function is constantly being run if audio is active
    // do not put any unnseceray code in it, unnsesecary=dealing with html elements
    // same as recorder_onaudioprocess

    let left_channel = e.outputBuffer.getChannelData(0);
    let right_channel = e.outputBuffer.getChannelData(1);

    if (g_audio_queue.length())
    {
        // the queue holds interleaved stereo (L R L R), two floats per output frame
        let interleaved = g_audio_queue.read(g_audio_config.codec.bufferSize * 2);
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

// ScriptProcessorNode capture fallback: runs on the MAIN thread; the worklet path delivers the
// same chunks through the capture node's port instead. both feed process_captured_microphone_pcm
function recorder_onaudioprocess(event)
{
    process_captured_microphone_pcm(event.inputBuffer.getChannelData(0));
}

// one chunk of captured microphone pcm (bufferSize mono samples), from either capture path;
// checks the datachannel is usable, then hands the samples to the opus g_encoder worker
function process_captured_microphone_pcm(webaudio_captured_bytes)
{
    if (typeof (g_datachannel) == 'undefined')
    {
        console.log("%c datachannel is undefined, cannot sent audio", "color: red");
        return;
    }

    if (g_datachannel == null)
    {
        return;
    }

    if (g_datachannel.readyState == null)
    {
        return;
    }

    if (g_datachannel.readyState != "open")
    {
        console.log("%c datachannel exists but is not open, cannot sent audio", "color: red");
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
