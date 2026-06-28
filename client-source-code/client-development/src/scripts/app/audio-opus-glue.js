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


            /** Callback when main thread receives a message */
            function mainthread_onmessage(e)
            {
                // console.log("%cmainthread_onmessage => " + e.data.type, "color: purple");

                if (e.data.type == "log")
                {
                    custom_log(e.data.value);
                }
                else if (e.data.type == "opus_encoder_worker__encode_result")
                {
                    if (current_channel_keys != null)
                    {
                        let opus_data_chunks = e.data.value;

                        for (var i = 0; i < opus_data_chunks.length; i++)
                        {
                            //console.log(opus_data_chunks[i]); useful for finding out if audio is sent
                            let data = encrypt_data_with_aes_keys(current_channel_keys, opus_data_chunks[i]);
                            g_datachannel.send(data);
                        }
                    }
                    else
                    {
                        custom_log("dont have channel keys, cant encrypt audio for sending");
                    }
                }
                else if (e.data.type == "opus_decoder_worker__decode_result")
                {
                    let webaudio_data_chunks = e.data.value;
                    audio_queue.write(webaudio_data_chunks);
                }
                else if (e.data.type == "minimp3_worker__decode_result")
                {
                    let data_chunks = chunkBuffers(e.data.value, 4096);

                    let message_object1 = {
                        message:
                        {
                            type: "start_song_stream",
                            song_name: selected_song_name
                        }
                    };

                    let message_json_string1 = process_message_before_sending(message_object1);
                    let data1 = encrypt_all_message_data_and_convert_to_base64(message_json_string1);
                    websocket_worker_send(data1);

                    stream_local_mp3_file_to_other_clients(data_chunks, e.data.mp3_sample_rate);

                    if (alert_streaming_music_shown_once == false)
                    {
                        custom_alert("when streaming music from file, detach browser tab where chat is to separate window (if using multiple tabs) or stay focused on tab where chat is otherwise music is not sent reliably and it lags");
                        alert_streaming_music_shown_once = true;
                    }
                }
                else if (e.data.type == "websocket_worker_onmessage")
                {
                    //data from websocket_worker are passed to main thread and then to data_processing_worker
                    data_processing_worker.postMessage({
                        type: "mainthread__process_received_websocket_message",
                        value: e.data.value
                    });
                }
                else if (e.data.type == "websocket_worker_onclose")
                {
                    if (is_authenticated)
                    {
                        if (is_running_in_android_webview == false)
                        {
                            custom_alert("connection with server was lost");
                        }
                        //window.location.reload();
                    }
                    is_authenticated = false;

                    reset_chat_app_keep_identity();
                }
                else if (e.data.type == "websocket_worker_onerror")
                {
                    if (is_running_in_android_webview == false)
                    {
                        custom_alert("connecting to server failed");

                        // window.alert("connection with server lost"); //this will be window.alert, this is serious problem
                    }
                    reset_chat_app_keep_identity();

                    // window.location.reload();
                    is_authenticated = false;
                }
                else if (e.data.type == "data_processing_worker__generate_rsa_keypair_result")
                {
                    rsa_public_key_string = e.data.value;
                    console.log("public key string -> " + rsa_public_key_string);
                    is_rsa_key_generated = true;
                    document.getElementById("another-buttons-sub-loading-container").style.display = "none";
                    document.getElementById("another-buttons-sub-container").style.display = "";
                    identity_string = e.data.identity_string;
                }
                else if (e.data.type == "data_processing_worker__authentication_status")
                {
                    if (e.data.value == "success")
                    {
                        hide_custom_alert(); //if there was any alert

                        if (e.data.is_idle_mode_allowed)
                        {
                           document.getElementById('channel-list-container').style.height = "calc(70% - 30px)";
                           document.getElementById("idle-channel-collapse-button").addEventListener("mousedown", UI.collapse_expand_channel);
                           document.getElementById('idle-clients-container').style.display = "block";
                        }

                        is_authenticated = true;

                        g_client_list = client_list;
                        g_channel_list = channel_list;

                        if (is_running_in_android_webview)
                        {
                            document.getElementById("android-settings-button").style.display = "none";
                        }

    
                        document.getElementById('verification-system').style.display = "none";
                        document.getElementById('communication-system-container').style.display = "block";

                        //people on touch device dont need to see chat by default, current UI isnt suitable for that
                        //most likely they will just want to call with voice
                        if (is_client_running_under_touch_device)
                        {
                            UI.hide_chat_container();
                        }

                        if (!should_connection_check_be_running)
                        {
                            should_connection_check_be_running = true;
                            websocket_connection_check(websocket);
                        }


                        //the connected sound effect is delayed for 1 second...
                        //because at this moment its not yet known if the sound effect should be played or not. why?
                        //because, at the moment client.html receives client_list message (later, shortly after this)
                        //client.html invokes JavaExport onconnected (if client.html is running in android)
                        //the invoke causes java to send user settings to androids WebView (client.html) 
                        //it happens in short time frame
                        //so thats why this timeout waits 1 second
                        //of course this is a bit useless in web browser context, but web browser can live with the sound effect delayed at 1 second
                        //plus minus 1 second doesnt matter in this case
                        //and the way its made now, android needs it
                        setTimeout( () => {
                            if (are_sound_effects_enabled)
                            {
                                sound_effects.connected.play();
                            }
                        }, 1000);
                    }
                }
                else if (e.data.type == "data_processing_worker__audio_enabled")
                {
                    //
                    //default stun port is 3478 for this chat. Stun port is given to client with websocket connection, stun port can be changed if needed, there are two places that need to be edited in code of server to set different stun port
                    //in stun_server.rs in this line let bind_address: SocketAddr = SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), 3478);
                    //in main.rs in beginning of this function: fn send_authentication_status_to_client(responder: &Responder)
                    //    let message_to_send: serde_json::Value = serde_json::json!({
                    //   "message" : {
                    //        "value": "success",
                    //  "stun_port": 3478
                    //    }
                    //});
                    //

                    iceconfig = {
                        iceServers: [{
                            urls: "stun:" + host + ":" + e.data.stun_port,
                        }],
                    };

                    //
                    //the datachannel/playback below is set up whenever audio is on for clients or music bots,
                    //so this flag (which keeps the datachannel established/reconnected) is always true here.
                    //whether this client may transmit its own microphone is a separate flag, gated on client voice
                    //
                    is_voice_chat_allowed_by_server = true;
                    is_client_microphone_allowed_by_server = (e.data.client_voice_allowed == true);
                    audio_context = new (window.AudioContext || window.webkitAudioContext)();
                    console.log("audio_context.sampleRate" + audio_context.sampleRate);

                    let opus_decoding_sampler_channels = 1;
                    let opus_decoding_sampler_input_rate = 48000;
                    let opus_decoding_sampler_output_rate = audio_context.sampleRate;

                    opus_decoding_sampler = new SpeexResampler(opus_decoding_sampler_channels,
                        opus_decoding_sampler_input_rate,
                        opus_decoding_sampler_output_rate);

                    opus_decoder_worker.postMessage({
                        type: "init",
                        sampleRate: opus_decoding_sampler_output_rate
                    });

                    silence = new Float32Array(audio_config.codec.bufferSize);
                    player = audio_context.createScriptProcessor(audio_config.codec.bufferSize, 1, 1);
                    player.onaudioprocess = player_onaudioprocess;
                    audio_player_gain_node = audio_context.createGain();
                    player.connect(audio_player_gain_node);
                    audio_player_gain_node.connect(audio_context.destination);

                    try
                    {
                       console.log("create_new_peer_connection_object_for_use");
                       is_is_webrtc_datachannel_check_running = true;
                       webrtc_datachannel_connection_check(false);
                    }
                    catch (Exception)
                    {
                       custom_log(Exception.toString());
                       custom_alert('audio connection failed');
                       return;
                    }
                }
                else if (e.data.type == "data_processing_worker__metadata_keys_accepted")
                {
                    keys_init_status = true;
                }
                else if (e.data.type == "data_processing_worker__client_list_from_server")
                {
                    server_msg.process_client_list_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__client_avatar_from_server")
                {
                    server_msg.process_client_avatar_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__connection_check_response")
                {
                    g_connection_check_message_response_received_timestamp = new Date().valueOf();
                }
                else if (e.data.type == "data_processing_worker__channel_list_from_server")
                {
                    server_msg.process_channel_list_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__tag_list_from_server")
                {
                    server_msg.process_tag_list_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__icon_list_from_server")
                {
                    server_msg.process_icon_list_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__icon_add_from_server")
                {
                    server_msg.process_icon_add_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__tag_add_to_client_from_server")
                {
                    server_msg.process_add_tag_to_client_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__remove_tag_from_client_from_server")
                {
                    server_msg.process_remove_tag_from_client_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__tag_add_from_server")
                {
                    server_msg.process_tag_add_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__start_song_stream_from_server")
                {
                    server_msg.process_start_song_stream_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__stop_song_stream_from_server")
                {
                    server_msg.process_stop_song_stream_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__chat_message_delete_from_server")
                {
                    server_msg.process_chat_message_delete_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__public_key_challenge_from_server")
                {
                    let msg = e.data.value;
                    let dh_public_mix_string = msg.message.dh_public_mix;
                    //console.log("DH dh_public_mix_string from server -> " + dh_public_mix_string);


                    let dh_public_mix = bigInt(dh_public_mix_string);

                    //reject a degenerate server public mix (B <= 1 or B >= p-1) before using it; such values
                    //force a known/tiny shared secret. for a safe prime, requiring 2 <= B <= p-2 is sufficient.
                    if (dh_public_mix.lesserOrEquals(1) || dh_public_mix.greaterOrEquals(dh_modulus.minus(1)))
                    {
                        console.error("rejected degenerate DH public mix from server; aborting handshake");
                        return;
                    }

                    let shared_secret = dh_public_mix.modPow(dh_secret_exponent, dh_modulus);


                    //add shared secret to
                    //console.log("DH shared_secret -> " + shared_secret.toString());

                    //
                    //because there is also "secret key" used ,used to further secure connection between server and client, and is different for every connected client, (obtained with steps that wants to look like diffie-hellman key exchange)
                    //this secret key needs to be added to metadata keys
                    //and then copy of metadata_keys object sent to data processing webworker aswell (there are two copies of same object)
                    //

                    let shared_secret_string = shared_secret.toString();

                    //derive the AES enc key + HMAC mac key from the shared secret via HKDF (matches the server)
                    let dh_keys = dh_derive_keys(shared_secret_string);
                    key_bytes = dh_keys.enc_key;

                    let iv_bytes = [90, 11, 8, 33, 4, 50, 50, 88, 8, 89, 200, 15, 24, 4, 15, 10];

                    let single_key = {
                        info: "aes-ctr",
                        key_string: shared_secret_string,
                        key_bytes: key_bytes,
                        iv_bytes: iv_bytes,
                        mac_bytes: dh_keys.mac_key
                    };

                    metadata_keys.unshift(single_key);

                    /* removed: never log key material (metadata_keys holds the session key) */

                    //
                    //here the metadata_keys object within data_processing web worker is updated aswell
                    //


                    console.log("public_key_challenge_response result -> ", msg.message.decryption_result);


                    data_processing_worker.postMessage({
                        type: "mainthread__metadata_keys",
                        value: metadata_keys
                    });


                    let message_object = {
                        message: {
                            type: "public_key_challenge_response",
                            value: msg.message.decryption_result
                        }
                    };

                    let message_json_string = process_message_before_sending(message_object);

                    let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

                    websocket_worker_send(data);
                }
                else if (e.data.type == "data_processing_worker__chat_message_edit_from_server")
                {
                    server_msg.process_chat_message_edit_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__poke_from_server")
                {
                    server_msg.process_poke_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__access_denied_from_server")
                {
                    server_msg.process_access_denied_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__client_info_from_server")
                {
                    server_msg.process_client_info_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_full_from_server")
                {
                    let full_channel = get_channel_by_id(channel_list, e.data.value.message.channel_id);
                    let full_channel_name = (full_channel != null && full_channel.name != null) ? full_channel.name : "channel";
                    custom_alert("'" + full_channel_name + "' is full");
                }
                else if (e.data.type == "data_processing_worker__server_settings_values_from_server")
                {
                    let msg = e.data.value;
                    document.getElementById("server-settings-general-display-flags-checkbox").checked = msg.message.display_country_flags == true;
                    document.getElementById("server-settings-general-enable-audio").checked = msg.message.enable_audio == true;
                    document.getElementById("server-settings-general-enable-music-bot-audio-checkbox").checked = msg.message.enable_music_bot_audio == true;
                    document.getElementById("server-settings-general-hide-clients-in-password-protected-channels").checked = msg.message.hide_clients_in_password_channels == true;
                    document.getElementById("server-settings-general-allow-temp-channels-checkbox").checked = msg.message.allow_temp_channels == true;
                    UI.render_bans_list(msg.message.bans);
                }
                else if (e.data.type == "data_processing_worker__channel_join_from_server")
                {
                    server_msg.process_channel_join_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__server_chat_message_id_for_local_message_id_from_server")
                {
                    server_msg.process_server_chat_message_id_for_local_message_id_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__client_connect_from_server")
                {
                    server_msg.process_client_connect_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__client_rename_from_server")
                {
                    server_msg.process_client_rename_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__client_disconnect_from_server")
                {
                    server_msg.process_client_disconnect_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__audio_state_of_single_client_from_server")
                {
                    //console.log(e.data.value);

                    let client = {
                        client_id: e.data.value.message.client_id,
                        audio_state: e.data.value.message.value,
                    }
                    process_audio_state_of_single_client(client);
                }
                else if (e.data.type == "data_processing_worker__current_channel_active_microphone_usage_from_server")
                {
                    console.log("data_processing_worker__current_channel_active_microphone_usage_from_server");

                    console.log(e.data.value);
                    for (var i = 0; i < e.data.value.message.clients.length; i++)
                    {
                        process_audio_state_of_single_client(e.data.value.message.clients[i]);
                    }
                }
                else if (e.data.type == "data_processing_worker__sdp_offer_from_server")
                {
                    server_msg.process_sdp_offer_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__ice_candidate_from_server")
                {
                    server_msg.process_ice_candidate_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__server_info_broadcast_from_server")
                {
                    server_msg.process_server_info_broadcast_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_edit_from_server")
                {
                    server_msg.process_channel_edit_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__client_going_to_idle_mode")
                {
                    server_msg.process_client_client_going_to_idle_mode_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__client_coming_back_from_idle_mode")
                {
                    server_msg.process_client_coming_back_from_idle_mode_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_delete_from_server")
                {
                    server_msg.process_channel_delete_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_create_from_server")
                {
                    server_msg.process_channel_create_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_chat_picture_metadata")
                {
                    server_msg.process_channel_chat_picture_metadata_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__direct_chat_picture_metadata")
                {
                    server_msg.process_direct_chat_picture_metadata_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_maintainer_id")
                {
                    server_msg.process_channel_maintainer_id_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_chat_message")
                {
                    server_msg.process_channel_chat_message_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__direct_chat_message")
                {
                    server_msg.process_direct_chat_message_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__direct_chat_picture")
                {
                    server_msg.process_direct_chat_picture_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__channel_chat_picture")
                {
                    server_msg.process_channel_chat_picture_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__image_sent_status_from_server")
                {
                    server_msg.process_image_sent_status_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__call_from_server")
                {
                    server_msg.process_call_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__create_websocket_channel_keys_message_result")
                {
                    console.log("sending channel keys to user -> " + e.data.username);
                    websocket_worker_send(e.data.channel_keys_message_content);
                }
                else if (e.data.type == "data_processing_worker__new_channel_keys_from_data_processing_worker")
                {
                    current_channel_keys = e.data.value;
                    
                    //data processing worker created new channel keys and sent them to UI thread, now send them from UI thread to opus_decoder worker
                    opus_decoder_worker.postMessage({
                        type: "mainthread__channel_keys_for_opus_decoder",
                        value: current_channel_keys
                    });
                }
                else if (e.data.type == "data_processing_worker__tell_websocket_worker_to_send_data")
                {
                    websocket_worker_send(e.data.data_to_be_sent_over_websocket);
                }
                else if (e.data.type == "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts")
                {
                    let total_bytes_length = e.data.data_for_upload_process.length;
                    let parts = split_string_into_smaller_parts(e.data.data_for_upload_process, 400);
                    file_send_intent = "direct_chat_picture_file";
                    file_send_intent_extra_data = e.data.extra_data;
                    send_file_by_parts(parts, total_bytes_length, 5);
                }
                else if (e.data.type == "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts")
                {
                    let total_bytes_length = e.data.data_for_upload_process.length;
                    let parts = split_string_into_smaller_parts(e.data.data_for_upload_process, 400);
                    file_send_intent = "channel_chat_picture_file";
                    file_send_intent_extra_data = e.data.extra_data;
                    send_file_by_parts(parts, total_bytes_length, 5);
                }
                else if (e.data.type == "data_processing_worker__music_bot_song_list_from_server")
                {
                    server_msg.process_music_bot_song_list_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__file_send_success_from_server")
                {
                    server_msg.process_file_send_success_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__file_receive_chunk_from_server")
                {
                    server_msg.process_file_receive_chunk_from_server_from_server(e.data.value);
                }
                else if (e.data.type == "data_processing_worker__file_receive_completed_from_server")
                {
                    server_msg.process_file_receive_completed_from_server_from_server(e.data.value);
                }
            }


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


            //this function gets run every 20ms. Can be extended to 100ms? possibly 200ms? But not more.
            //this function handles merging of multiple opus streams into single pcm from very high level perspective
            function opus_decoder_worker_interval_function() 
            {
                if (opus_decoder_worker_clients_opus_data_count == 0)
                {
                    return;
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

                //console.log("longest array is ", longestLength);
                let clear_buffered_pcm_needed = 0;

                //this is how loop works in practice, runs every 20ms, needs testing
                //loop through clients 1, 2, 3, get array with index 0 if it exists, and mix it
                //at the end, send decoded mixed pcm to webaudioplayer
                //loop throguh clients 1, 2, 3, get array with index 1 if it exists  and mix it
                //at the end, send decoded mixed pcm to webaudioplayer
                //loop throguh clients 1, 2, 3, get array with index 2 if it exists  and mix it
                //at the end, send decoded mixed pcm to webaudioplayer

                for (let ArrayBuffer_index = 0; ArrayBuffer_index < longestLength; ArrayBuffer_index++)
                {
                    for (let client_index = 0; client_index < opus_decoder_worker_current_channel_opus_client_ids.length; client_index++)
                    {
                        if (client_index == 0)
                        {
                            //client_index equals zero means pcm buffer should be cleared
                            //BUT, only actual decoding in case ArrayBuffer isnt null, sets clear_buffered_pcm_needed to 0 again
                            clear_buffered_pcm_needed = 1;
                        }
                        
                        //skip single_opus_chunk if client doesnt have element at that index
                        if (ArrayBuffer_index >= opus_decoder_worker_clients_opus_data[client_index].length)
                        {
                            continue;
                        }

                        let single_opus_chunk = opus_decoder_worker_clients_opus_data[client_index][ArrayBuffer_index];
                        decoder.decode(single_opus_chunk, clear_buffered_pcm_needed);
                        clear_buffered_pcm_needed = 0;
                    }

                    let pulse_code_modulation_bytes_for_webaudio = decoder.getPcmBuffer();
                    
                    global.postMessage({
                        type: "opus_decoder_worker__decode_result",
                        value: pulse_code_modulation_bytes_for_webaudio
                    });
                }

                opus_decoder_worker_clients_opus_data.length = 0;
                opus_decoder_worker_clients_opus_data_count = 0;
                opus_decoder_worker_current_channel_opus_client_ids.length = 0;
                opus_decoder_worker_current_channel_opus_client_ids_map.clear();
            }

            function opus_decoder_worker_onmessage(event)
            {
                //add_for_decoding decrypts opus data and adds it to opus_decoder_worker_clients_opus_data , array of buffers..
                if (event.data.type == "mainthread__add_data_to_opus_decoder")
                {
                    let dataView = new DataView(event.data.value);

                    //clientid is always first 4 bytes of received chunk of bytes and is in every chunk of bytes
                    let extracted_client_id = dataView.getInt32(0, true);

                    //check if client id is known
                    if (!opus_decoder_worker_current_channel_opus_client_ids.includes(extracted_client_id))
                    {
                        opus_decoder_worker_current_channel_opus_client_ids.push(extracted_client_id);
                        opus_decoder_worker_current_channel_opus_client_ids.forEach((clientId, index) => {
                            opus_decoder_worker_current_channel_opus_client_ids_map.set(clientId, index);
                        });

                        let client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(extracted_client_id);
                        opus_decoder_worker_clients_opus_data[client_index] = [];
                    }

                    let client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(extracted_client_id);
                    let opus_ArrayBuffer_encrypted = event.data.value.slice(4);
                    let opus_ArrayBuffer = decrypt_data_with_aes_keys(current_channel_keys, opus_ArrayBuffer_encrypted);
                    
                    //console.log(opus_ArrayBuffer);
                    //console.log(opus_decoder_worker_clients_opus_data[client_index]);
                    opus_decoder_worker_clients_opus_data[client_index].push(opus_ArrayBuffer);
                    opus_decoder_worker_clients_opus_data_count = opus_decoder_worker_clients_opus_data_count + 1;

                    //check if client id is known (in other words if it has been encountered before in clients current channel)
                    // if not, push new client id to array of client ids and get client index in array for the client id,
                    // that will be used to store opus data chunks
                    //use client index to create object that will hold opus data
                }
                else if (event.data.type == "mainthread__add_data_to_opus_decoder_musicbot")
                {
                    let dataView = new DataView(event.data.value);

                    let extracted_client_id = dataView.getInt32(0, true);

                    if (!opus_decoder_worker_current_channel_opus_client_ids.includes(extracted_client_id))
                    {
                        opus_decoder_worker_current_channel_opus_client_ids.push(extracted_client_id);
                        opus_decoder_worker_current_channel_opus_client_ids.forEach((clientId, index) => {
                            opus_decoder_worker_current_channel_opus_client_ids_map.set(clientId, index);
                        });

                        let client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(extracted_client_id);
                        opus_decoder_worker_clients_opus_data[client_index] = [];
                    }

                    let client_index = opus_decoder_worker_current_channel_opus_client_ids_map.get(extracted_client_id);
                    let opus_ArrayBuffer = event.data.value.slice(4);
                    //console.log(opus_ArrayBuffer);


                    opus_decoder_worker_clients_opus_data[client_index].push(opus_ArrayBuffer);
                    opus_decoder_worker_clients_opus_data_count = opus_decoder_worker_clients_opus_data_count + 1;     
                }
                else if (event.data.type == "mainthread__channel_keys_for_opus_decoder")
                {
                    current_channel_keys = event.data.value;
                    //console.log("mainthread__channel_keys_for_opus_decoder got channel keys", current_channel_keys);
                }
                else if (event.data.type == "init")
                {
                    // opus_decoder_worker_interval = window.setInterval(opus_decoder_worker_interval_function, 20);

                    //how can setInterval even work? This is webworker.. it should not have setInterval.. 

                    opus_decoder_worker_interval = setInterval(opus_decoder_worker_interval_function, 20);

                    decoder = new OpusDecoder(48000, 1);
                }
            }

                     //