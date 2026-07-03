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

                //sample rate must be one of 8000, 12000, 16000, 24000, or 48000.

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

            //invalidates audio data still in buffer
            //added because when person push to talk, some leftover data remained there
            //experimental, (didnt work)
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

            OpusEncoder.prototype.encode = function (samples)
            {
                //if resample is needed or not, is known right at the beginning, based on clients microphone
                //so only one resampler is used here, compared to encode_mp3_chunk function

                //if resampling is not done right, persons voice will be either high pitched, or too low

                var size;
                var ret;
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

            OpusEncoder.prototype.encode_mp3_chunk = function (samples, input_pcm_sample_rate)
            {
                //goal of this function is to make sure PCM is converted to 48kHz if it needs to be
                //problem is, each mp3 file has different sample rate, some are 44.1kHz, some are 48

                var size;
                var ret;
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

            OpusEncoder.prototype.destroy = function ()
            {
                _opus_encoder_destroy(this.handle);
                if (this.resampler)
                {
                    this.resampler.destroy();
                }

                if (this.resampler_44100kHz_to_48000kHz)
                {
                    this.resampler.destroy();
                }
                
                this.handle = null;
                this.buf = null;
                this.pcm_buffer_for_mixing = null;
            }

            function OpusDecoder(sampleRate, channels)
            {
                this.channels = channels;
                var err = Module._malloc(4);
                this.handle = _opus_decoder_create(sampleRate, this.channels, err);
                var errNum = Module.getValue(err, "i32");
                Module._free(err);
                if (errNum != 0)
                {
                    console.log("there is error");
                    return;
                }

                this.frameSize = sampleRate * 60 / 1000;
                var bufSize = 1275 * 3 + 7;
                var pcmSamples = this.frameSize * this.channels;

                this.bufSize = bufSize;
                this.bufPtr = Module._malloc(bufSize);
                this.buf = Module.HEAPU8.subarray(this.bufPtr, this.bufPtr + bufSize);


                this.pcmBufferSize = 4 * pcmSamples;

                //Module.HEAPF32.subarray creates a view for allocated buffer. Used when creating Float32Array later at some point
                this.pcmPtr = Module._malloc(this.pcmBufferSize);
                this.pcm = Module.HEAPF32.subarray(this.pcmPtr / 4, this.pcmPtr / 4 + pcmSamples);

                //console.log("OpusDecoder this.pcmBufferSize" , this.pcmBufferSize);
                //console.log("OpusDecoder this.pcmPtr / 4" , this.pcmPtr / 4);

                this.pcm_buffer_for_mixing_ptr = Module._malloc(this.pcmBufferSize);
                this.pcm_buffer_for_mixing = Module.HEAPF32.subarray(0, this.pcmBufferSize / 4);

                this.highestPcmCountEncountered = 0; //this member variable exists so getPcmBuffer knows, how big is returning buffer going to be
            }

            OpusDecoder.prototype.getPcmBuffer = function()
            {
                let aa = new Float32Array(this.pcm_buffer_for_mixing.subarray(0, this.highestPcmCountEncountered * this.channels));
                this.highestPcmCountEncountered = 0;
                return aa;
            }

            //re-derives the wasm-heap views from the current heap buffer. the views cached at construction
            //time detach if the emscripten heap grows during a later instance's allocation, so after building
            //the whole decoder pool every member's views are refreshed once, when no further mallocs follow
            OpusDecoder.prototype.refresh_heap_views = function()
            {
                this.buf = Module.HEAPU8.subarray(this.bufPtr, this.bufPtr + this.bufSize);
                this.pcm = Module.HEAPF32.subarray(this.pcmPtr / 4, this.pcmPtr / 4 + this.frameSize * this.channels);
                this.pcm_buffer_for_mixing = Module.HEAPF32.subarray(0, this.pcmBufferSize / 4);
            }

            OpusDecoder.prototype.decode = function (payload, clear_buffered_pcm)
            {
                /* edited 29.6.2025
                    payload = audio bytes decrypted with maintainer's channel key on clients end (this end)
                    _opus_decode_float is located in opus_decoder.c
                */

                this.buf.set(new Uint8Array(payload));
                var ret = _opus_decode_float(this.handle, this.bufPtr, payload.byteLength, this.pcmPtr, this.pcmBufferSize, this.pcm_buffer_for_mixing, clear_buffered_pcm, this.frameSize, 0);

                if (ret > this.highestPcmCountEncountered)
                {
                    this.highestPcmCountEncountered = ret;
                }

                return ret;
            }

            OpusDecoder.prototype.destroy = function ()
            {
                _opus_decoder_destroy(this.handle);
                this.handle = null;
                this.buf = null;
                this.pcm = null;
            }

            var IS_WORKER = !global.document && !!global.postMessage;
            var IS_CURRENT_THREAD_WORKER = IS_WORKER && /blob:/i.test((global.location || {}).protocol);


            function create_new_webworker_in_same_file(worker_name)
            {
                console.log("trying to create webworker " + worker_name);
                let URL = global.URL || global.webkitURL || null;
                let code = moduleFactory.toString();

                //
                //when creating new webworker within same file, get the string that represents code
                //alter the string, add variable to it with name of the worker
                //

                let string_to_find = "var THREAD_NAME = "; //first offurence is this variable
                let first_occurence_index = code.indexOf(string_to_find);
                let replace_start_index = code.indexOf(string_to_find, first_occurence_index + 1);
                replace_start_index = replace_start_index + string_to_find.length;

                code = code.substring(0, replace_start_index) + "" + "\"" + worker_name + "" + code.substring(replace_start_index + worker_name.length, code.length);

                let worker_url = URL.createObjectURL(new Blob(['(', code, ')();'], { type: 'text/javascript' }));
                let newly_created_worker = new global.Worker(worker_url);
                newly_created_worker.onmessage = mainthread_onmessage;
                newly_created_worker.worker_name = worker_name;
                return newly_created_worker;
            }


            /* mainthread_onmessage (the main-thread worker-message dispatcher) lives in main.js */


            //e.data.value must be Float32Array
            function opus_encoder_worker_onmessage(e)
            {
                if (e.data.type == "encode")
                {
                    //let resampled = opus_encoding_sampler.process(e.data.value);
                    let opus_data_chunks = encoder.encode(e.data.value);

                    global.postMessage({
                        type: "opus_encoder_worker__encode_result",
                        value: opus_data_chunks
                    });
                }
                else if (e.data.type == "clear_opus_encoder_buffer")
                {
                    //let resampled = opus_encoding_sampler.process(e.data.value);
                    //encoder.reset();
                }
                else if (e.data.type == "encode_mp3_chunk")
                {
                    //let resampled = opus_encoding_sampler.process(e.data.value);
                    let opus_data_chunks = encoder.encode_mp3_chunk(e.data.value, e.data.mp3_sample_rate);

                    global.postMessage({
                        type: "opus_encoder_worker__encode_result",
                        value: opus_data_chunks
                    });
                }
                else if (e.data.type == "init")
                {
                    let encoder_application_use = 2048; //VOIP use
                    let encoder_frame_duration = 40; //Opus can encode frames of 2.5, 5, 10, 20, 40, or 60 ms
                    let encoder_channels = 1;
                    let encoder_output_samplerate = 48000; //if unsupported sample rate is used, Encoder wont construct
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
            var opus_decoder_worker_clients_opus_data_count = 0; //im keeping count in separate int varaible for simplier access
            var opus_decoder_worker_current_channel_opus_client_ids = [];
            var opus_decoder_worker_current_channel_opus_client_ids_map = new Map();

            //
            //per-sender decoder pool. opus is stateful (each frame is predicted from the same stream's previous
            //frame), so every concurrently-audible sender needs its own decoder - pushing interleaved streams
            //through one decoder corrupts the predictor state of all of them. the pool is allocated ONCE at init
            //(a mid-run malloc can grow the wasm heap and detach the cached heap views) and is never freed:
            //"releasing" a decoder just returns its slot to the free list, and the codec state is scrubbed in
            //place with OPUS_RESET_STATE when the slot is handed to a new sender. sized to simultaneous
            //SPEAKERS, not channel population - past a handful of concurrent voices the mix is noise anyway
            //
            var OPUS_DECODER_POOL_SIZE = 16;
            var OPUS_DECODER_IDLE_TICKS_BEFORE_RELEASE = 250; //250 ticks x 20ms = 5s of silence
            var OPUS_CTL_RESET_STATE = 4028; //OPUS_RESET_STATE from opus_defines.h
            var opus_decoder_pool = [];
            var opus_decoder_pool_free_indices = [];
            var opus_decoder_sender_map = new Map(); //sender client_id -> { pool_index, last_used_tick }
            var opus_decoder_tick_counter = 0;
            var opus_decoder_mix_scratch = null; //Float32Array(frameSize), created at init after the pool

            //receive-side sequence telemetry (frames lost on the unordered/unreliable datachannel)
            var opus_decoder_worker_lost_frame_count = 0;
            var opus_decoder_worker_late_frame_count = 0;
            var opus_decoder_worker_last_logged_lost_frame_count = 0;

            //direct mode: one end of a MessageChannel whose other end sits inside the player worklet.
            //while set, chunks are decoded on arrival and their pcm goes straight to the worklet
            //(per sender, no mixing here) - no 20 ms tick, no main-thread hop. null = tick/fallback mode
            var opus_decoder_direct_worklet_port = null;

            //returns the sender's decoder, claiming a pool slot on first use. on pool exhaustion the
            //longest-idle sender's slot is stolen (that one stream restarts cleanly on its next frame,
            //everyone else stays untouched)
            function opus_decoder_worker_get_decoder_for_sender(sender_client_id)
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
                        return null;
                    }

                    pool_index = opus_decoder_sender_map.get(oldest_sender).pool_index;
                    opus_decoder_sender_map.delete(oldest_sender);
                }

                //scrub the previous occupant's predictor state in place - no malloc/free involved
                _opus_decoder_ctl(opus_decoder_pool[pool_index].handle, OPUS_CTL_RESET_STATE);

                opus_decoder_sender_map.set(sender_client_id, {
                    pool_index: pool_index,
                    last_used_tick: opus_decoder_tick_counter,
                    last_sequence_number: null,
                    consecutive_stale_count: 0
                });
                return opus_decoder_pool[pool_index];
            }

            //returns slots of senders that have been silent for a while back to the free list.
            //no state is touched here; the reset happens when the slot is reassigned
            function opus_decoder_worker_release_idle_decoders()
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


            //this function gets run every 20ms. Can be extended to 100ms? possibly 200ms? But not more.
            //this function handles merging of multiple opus streams into single pcm from very high level perspective
            //decodes one chunk with THAT SENDER'S pooled decoder, applying the per-sender sequence rules
            //(duplicate/late drop, loss counting, resync after a counter restart). returns a fresh
            //Float32Array of interleaved stereo pcm, or null when the chunk was dropped.
            //used by BOTH modes: the direct pipe decodes on arrival, the tick mixer per 20 ms round
            function opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry)
            {
                let sender_decoder = opus_decoder_worker_get_decoder_for_sender(sender_client_id);

                if (sender_decoder == null)
                {
                    return null;
                }

                let sender_mapping = opus_decoder_sender_map.get(sender_client_id);

                //sequence handling: drop duplicates and late frames, count losses. a long run of
                //"late" frames means the sender restarted its counter (reconnect) - resync to it
                if (chunk_entry.sequence_number != null && sender_mapping.last_sequence_number != null)
                {
                    let sequence_delta = (chunk_entry.sequence_number - sender_mapping.last_sequence_number) & 0xffff;

                    if (sequence_delta == 0)
                    {
                        return null; //duplicate frame
                    }

                    if (sequence_delta >= 0x8000)
                    {
                        opus_decoder_worker_late_frame_count = opus_decoder_worker_late_frame_count + 1;
                        sender_mapping.consecutive_stale_count = sender_mapping.consecutive_stale_count + 1;

                        //25 stale frames in a row (~0.5 s of audio): not reordering, the sender's
                        //counter restarted - fall through and resync to it
                        if (sender_mapping.consecutive_stale_count <= 25)
                        {
                            return null;
                        }
                    }
                    else if (sequence_delta > 1)
                    {
                        let lost_now = sequence_delta - 1;

                        if (lost_now > 250)
                        {
                            lost_now = 250; //a jump this big is a resync, not real loss; keep the stat sane
                        }

                        opus_decoder_worker_lost_frame_count = opus_decoder_worker_lost_frame_count + lost_now;

                        if (opus_decoder_worker_lost_frame_count - opus_decoder_worker_last_logged_lost_frame_count >= 100)
                        {
                            console.log("audio receive: " + opus_decoder_worker_lost_frame_count + " frames lost, " + opus_decoder_worker_late_frame_count + " late/duplicate so far");
                            opus_decoder_worker_last_logged_lost_frame_count = opus_decoder_worker_lost_frame_count;
                        }
                    }
                }

                if (chunk_entry.sequence_number != null)
                {
                    sender_mapping.last_sequence_number = chunk_entry.sequence_number;
                    sender_mapping.consecutive_stale_count = 0;
                }

                let decoded_sample_count = sender_decoder.decode(chunk_entry.opus_chunk, 1);

                if (decoded_sample_count <= 0)
                {
                    return null;
                }

                //decode returns samples PER CHANNEL; the shared frame buffer holds interleaved stereo
                return new Float32Array(sender_decoder.pcm_buffer_for_mixing.subarray(0, decoded_sample_count * sender_decoder.channels));
            }

            //direct mode: decode one sender's chunk immediately (no 20 ms tick latency) and transfer the
            //pcm straight to the player worklet, which jitter-buffers per sender and mixes on the audio clock
            function opus_decoder_worker_decode_and_pipe(sender_client_id, chunk_entry)
            {
                let decoded_pcm = opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry);

                if (decoded_pcm == null)
                {
                    return;
                }

                opus_decoder_direct_worklet_port.postMessage({
                    id: sender_client_id,
                    pcm: decoded_pcm
                }, [decoded_pcm.buffer]);
            }

            //fallback (tick) mode: register the sender for this 20 ms round and queue the chunk for the mixer
            function opus_decoder_worker_queue_chunk_for_tick(sender_client_id, chunk_entry)
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

            //direct-pipe mode housekeeping: the 20 ms mixing tick is gone, but idle decoder slots still
            //need returning to the pool. the tick counter advances by 50 per second to keep the
            //"1 tick = 20 ms" units of last_used_tick / OPUS_DECODER_IDLE_TICKS_BEFORE_RELEASE valid
            function opus_decoder_worker_housekeeping_function()
            {
                opus_decoder_tick_counter = opus_decoder_tick_counter + 50;
                opus_decoder_worker_release_idle_decoders();
            }

            function opus_decoder_worker_interval_function()
            {
                opus_decoder_tick_counter = opus_decoder_tick_counter + 1;

                //cheap periodic sweep: hand slots of long-silent senders back to the pool
                if ((opus_decoder_tick_counter & 63) == 0)
                {
                    opus_decoder_worker_release_idle_decoders();
                }

                if (opus_decoder_worker_clients_opus_data_count == 0)
                {
                    return;
                }

                //order each sender's chunks by sequence number (wrap-aware around 65536) so frames the
                //unordered datachannel delivered scrambled are decoded in capture order. legacy chunks
                //without a sequence number keep arrival order
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

                //finf longest Array of ArrayBuffers. Who has it? the longest array length will be n times
                //loop n times, for each client, check if he has said index too, if he has it, merge it,


                let longestLength = 0;

                //find longest array that holds most ArrayBuffers (opus chunks)
                for (let client_index = 0; client_index < opus_decoder_worker_current_channel_opus_client_ids.length; client_index++)
                {
                    let current_length = opus_decoder_worker_clients_opus_data[client_index].length;
                    if (longestLength < current_length)
                    {
                        longestLength = current_length;
                    }
                }

                //per time step: decode each sender's chunk with THAT SENDER'S decoder (predictor state stays
                //per-stream), sum the frames in js, clamp to [-1,1], post one mixed block - same message the
                //main thread always consumed. decode is called with clear=1 so the shared frame buffer holds
                //exactly the current call's frame, which is accumulated into the scratch before the next call
                for (let ArrayBuffer_index = 0; ArrayBuffer_index < longestLength; ArrayBuffer_index++)
                {
                    let mixed_sample_count = 0;

                    opus_decoder_mix_scratch.fill(0);

                    for (let client_index = 0; client_index < opus_decoder_worker_current_channel_opus_client_ids.length; client_index++)
                    {
                        //skip single_opus_chunk if client doesnt have element at that index
                        if (ArrayBuffer_index >= opus_decoder_worker_clients_opus_data[client_index].length)
                        {
                            continue;
                        }

                        let sender_client_id = opus_decoder_worker_current_channel_opus_client_ids[client_index];
                        let chunk_entry = opus_decoder_worker_clients_opus_data[client_index][ArrayBuffer_index];

                        //sequence rules + per-sender decode live in the shared helper (same one the
                        //direct pipe uses); null means the chunk was dropped (duplicate/late/corrupt)
                        let decoded_pcm = opus_decoder_worker_decode_sender_chunk(sender_client_id, chunk_entry);

                        if (decoded_pcm == null)
                        {
                            continue;
                        }

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

                    //clamp-copy: summed streams can overshoot [-1,1]; the copy is freshly allocated so its
                    //buffer is transferred instead of structure-cloned on the worker -> main hop
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

            function opus_decoder_worker_onmessage(event)
            {
                //voice frame: [4B sender client id][encrypted(2B sequence + opus)]
                if (event.data.type == "mainthread__add_data_to_opus_decoder")
                {
                    let dataView = new DataView(event.data.value);

                    //clientid is always first 4 bytes of received chunk of bytes and is in every chunk of bytes
                    let extracted_client_id = dataView.getInt32(0, true);

                    let opus_ArrayBuffer_encrypted = event.data.value.slice(4);
                    let decrypted_bytes = decrypt_data_with_aes_keys(current_channel_keys, opus_ArrayBuffer_encrypted);

                    //after decryption: [2B sequence little endian][opus]. too-short frames are dropped
                    if (decrypted_bytes == null || decrypted_bytes.length < 3)
                    {
                        return;
                    }

                    let chunk_entry = {
                        sequence_number: decrypted_bytes[0] | (decrypted_bytes[1] << 8),
                        opus_chunk: decrypted_bytes.subarray(2)
                    };

                    //direct mode decodes right now and pipes to the worklet; tick mode queues for the mixer
                    if (opus_decoder_direct_worklet_port != null)
                    {
                        opus_decoder_worker_decode_and_pipe(extracted_client_id, chunk_entry);
                        return;
                    }

                    opus_decoder_worker_queue_chunk_for_tick(extracted_client_id, chunk_entry);
                }
                else if (event.data.type == "mainthread__add_data_to_opus_decoder_musicbot")
                {
                    let dataView = new DataView(event.data.value);
                    let extracted_client_id = dataView.getInt32(0, true);
                    let chunk_entry = null;

                    if (extracted_client_id == -2)
                    {
                        //legacy format from an old server binary: [4B -2][opus], no sequence number
                        chunk_entry = {
                            sequence_number: null,
                            opus_chunk: event.data.value.slice(4)
                        };
                    }
                    else
                    {
                        //new format: [4B bot client id][2B sequence little endian][opus]
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
                        opus_decoder_worker_decode_and_pipe(extracted_client_id, chunk_entry);
                        return;
                    }

                    opus_decoder_worker_queue_chunk_for_tick(extracted_client_id, chunk_entry);
                }
                else if (event.data.type == "mainthread__channel_keys_for_opus_decoder")
                {
                    current_channel_keys = event.data.value;
                    //console.log("mainthread__channel_keys_for_opus_decoder got channel keys", current_channel_keys);
                }
                else if (event.data.type == "use_direct_worklet_pipe")
                {
                    //the main thread transferred one end of a MessageChannel whose other end sits inside the
                    //player worklet: from here on, decode on arrival and pipe per-sender pcm directly - the
                    //20 ms mixing tick and the main-thread hop are gone. the interval becomes slow housekeeping
                    opus_decoder_direct_worklet_port = event.data.port;

                    clearInterval(opus_decoder_worker_interval);
                    opus_decoder_worker_interval = setInterval(opus_decoder_worker_housekeeping_function, 1000);

                    //drop anything the tick queue still holds; the pipe owns playback from here
                    opus_decoder_worker_clients_opus_data.length = 0;
                    opus_decoder_worker_clients_opus_data_count = 0;
                    opus_decoder_worker_current_channel_opus_client_ids.length = 0;
                    opus_decoder_worker_current_channel_opus_client_ids_map.clear();

                    console.log("opus decoder worker: direct worklet pipe active");
                }
                else if (event.data.type == "init")
                {
                    // opus_decoder_worker_interval = window.setInterval(opus_decoder_worker_interval_function, 20);

                    //how can setInterval even work? This is webworker.. it should not have setInterval..

                    opus_decoder_worker_interval = setInterval(opus_decoder_worker_interval_function, 20);

                    //allocate the whole per-sender decoder pool up front - every wasm malloc happens here,
                    //before any audio flows, so the heap never grows mid-run under the cached views.
                    //decoders are STEREO: an opus decoder's channel count is independent of the packet's -
                    //stereo music-bot packets decode as true stereo, mono voice packets get duplicated to
                    //both channels by libopus itself. everything downstream is interleaved L R L R
                    for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
                    {
                        opus_decoder_pool.push(new OpusDecoder(48000, 2));
                        opus_decoder_pool_free_indices.push(pool_index);
                    }

                    //allocating a later pool member may have grown the heap and detached the views the
                    //earlier members cached at construction - re-derive them all now that mallocs are done
                    for (let pool_index = 0; pool_index < OPUS_DECODER_POOL_SIZE; pool_index++)
                    {
                        opus_decoder_pool[pool_index].refresh_heap_views();
                    }

                    opus_decoder_mix_scratch = new Float32Array(opus_decoder_pool[0].frameSize * opus_decoder_pool[0].channels);
                }
            }

                     //