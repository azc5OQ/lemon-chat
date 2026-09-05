// dispatch.js is embedded in template.html along with the other client files, and in the node bundle
// it is the main thread's message dispatcher: everything the workers post back (decrypted server
// messages, encryption results, socket events) arrives in dispatch__mainthread_onmessage and is routed to the
// handlers in messages.js and the feature files

/**
 * @brief the main thread's message handler for everything the workers post back
 *        decrypted server messages go to their server_msg handler, encoder output to the
 *        datachannel, log lines to the log, and so on, one branch per message type
 *
 * @param object e -> the worker message event; e.data.type picks the branch
 *
 * @return void
 */
function dispatch__mainthread_onmessage(e)
{
    if (DBG_WORKER_BOOT_LOG) { console.log("[m<-w] " + (e.data && e.data.type)); }

    if (e.data.type == "log")
    {
        utils__custom_log(e.data.value);
    }
    else if (e.data.type == "opus_encoder_worker__encode_result")
    {
        // chunks already inside the encoder when push-to-talk ended arrive here after
        // mic-off; dropping them keeps the stale tail out of receivers' jitter lanes
        if (g_is_microphone_active == false && (typeof is_playing_music == "undefined" || is_playing_music != true))
        {
            return;
        }

        if (g_current_channel_keys != null)
        {
            let opus_data_chunks = e.data.value;

            for (var i = 0; i < opus_data_chunks.length; i++)
            {
                // prepend a 2-byte little-endian sequence number INSIDE the encrypted payload;
                // receivers use it to reorder frames the unordered datachannel scrambled and to
                // detect losses. the server relay never sees it (it sits under the encryption)
                let opus_chunk_bytes = new Uint8Array(opus_data_chunks[i]);
                let sequenced_chunk = new Uint8Array(2 + opus_chunk_bytes.length);

                sequenced_chunk[0] = g_voice_send_sequence_number & 0xff;
                sequenced_chunk[1] = (g_voice_send_sequence_number >> 8) & 0xff;
                sequenced_chunk.set(opus_chunk_bytes, 2);
                g_voice_send_sequence_number = (g_voice_send_sequence_number + 1) & 0xffff;

                let data = keys__encrypt_data_with_aes_keys(g_current_channel_keys, sequenced_chunk);
                if (data != null && data.byteLength > 0) { g_session.bytes_sent += data.byteLength; }
                g_datachannel.send(data);
            }
        }
        else
        {
            utils__custom_log("dont have channel keys, cant encrypt audio for sending");
        }
    }
    else if (e.data.type == "opus_decoder_worker__decode_result")
    {
        audio__audio_player_write_chunk(e.data.value);
    }
    else if (e.data.type == "minimp3_worker__decode_result")
    {
        let data_chunks = voice__chunk_buffers(e.data.value, 4096);

        let message_object1 = {
            message:
            {
                type: "start_song_stream",
                song_name: g_selected_song_name
            }
        };

        connection__send_message_object(message_object1);

        voice__stream_local_mp3_file_to_other_clients(data_chunks, e.data.mp3_sample_rate);

        if (g_alert_streaming_music_shown_once == false)
        {
            utils__custom_alert("when streaming music from file, detach browser tab where chat is to separate window (if using multiple tabs) or stay focused on tab where chat is otherwise music is not sent reliably and it lags");
            g_alert_streaming_music_shown_once = true;
        }
    }
    else if (e.data.type == "websocket_worker_onmessage")
    {
        if (e.data.value != null && e.data.value.length > 0) { g_session.bytes_received += e.data.value.length; }

        // data from g_websocket_worker are passed to main thread and then to g_data_processing_worker
        g_data_processing_worker.postMessage({
            type: "mainthread__process_received_websocket_message",
            value: e.data.value,
            // loopback frames arrive as plain json, the worker skips decryption
            is_plaintext: android_host__is_ui_only_runtime()
        });
    }
    else if (e.data.type == "data_processing_worker__rsa_key_too_weak")
    {
        keys__handle_rsa_key_too_weak_notice(e.data.minimum_rsa_key_bits);
    }
    else if (e.data.type == "websocket_worker_onclose")
    {
        // the driver awaits this signal; it decides whether and when to redial
        g_last_connect_attempt_failed = true;

        // the server never says why - these are inferences from WHERE the socket died.
        // in the webview the socket is the loopback to node, so its close says nothing
        // about the real connection: node owns that story and reports it separately
        if (android_host__is_ui_only_runtime() == false)
        {
            if (g_is_authenticated == true)
            {
                g_connection_status.last_connected_at = new Date().valueOf();

                if (g_last_disconnect_reason === "")
                {
                    g_last_disconnect_reason = "the server closed the connection (it probably kicked this client, or shut down)";
                }
            }
            else if (g_last_disconnect_reason === "")
            {
                g_last_disconnect_reason = (g_device_has_network === false)
                    ? "no network connection (wifi and mobile data are off)"
                    : "the server probably rejected the keys (it closed the connection during login)";
            }
        }

        connection__signal_connection_closed();
        console.warn("server connection lost (identity switch in progress: " + g_is_identity_switch_in_progress + ")");

        // the resume socket itself died: that is the one allowed failure, back to the connect screen
        if (g_fast_reconnect.in_progress == true)
        {
            connection__fast_reconnect_failed("the resume socket closed");
        }
        else if (connection__try_start_fast_reconnect("socket closed") == false)
        {
            // a deliberate close (identity switch) is not a lost connection - no scary alert
            if (g_is_authenticated && g_is_identity_switch_in_progress == false)
            {
                if (g_is_running_in_android_webview == false)
                {
                    utils__custom_alert("connection with server was lost");
                }
            }
            g_is_authenticated = false;

            connection__reset_chat_app_keep_identity();
        }
    }
    else if (e.data.type == "websocket_worker_onerror")
    {
        g_last_connect_attempt_failed = true;

        if (g_last_disconnect_reason === "")
        {
            g_last_disconnect_reason = connection__describe_socket_error(e.data.error_code);
        }

        if (g_is_authenticated == true)
        {
            g_connection_status.last_connected_at = new Date().valueOf();
        }

        connection__signal_connection_closed();

        if (g_fast_reconnect.in_progress == true)
        {
            connection__fast_reconnect_failed("the resume socket failed");
        }
        else if (connection__try_start_fast_reconnect("socket error") == false)
        {
            if (g_is_running_in_android_webview == false)
            {
                utils__custom_alert("connecting to server failed");
            }
            connection__reset_chat_app_keep_identity();
            g_is_authenticated = false;
        }
    }
    else if (e.data.type == "data_processing_worker__generate_rsa_keypair_result")
    {
        g_rsa_public_key_string = e.data.value;
        console.log("public key string -> " + g_rsa_public_key_string);
        g_is_rsa_key_generated = true;

        // the keygen phase is over: stop claiming it. when nothing will dial on its own the
        // status goes idle (empty), otherwise the driver's "connecting" replaces it right away
        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            connection__report_connection_status("idle", "");
        }
        document.getElementById("another-buttons-sub-loading-container").style.display = "none";
        document.getElementById("another-buttons-sub-container").style.display = "";
        g_identity_string = e.data.identity_string;

        // wakes the driver if it is waiting to dial with this identity
        g_identity_slot.set({ public_key_string: e.data.value, identity_string: e.data.identity_string });
        g_is_identity_switch_in_progress = false;

        // persist the identity (the passphrase) so the next launch reconstructs this same keypair
        // instead of a fresh random one. covers first launch (random) and identity switches alike:
        // the last identity used is the one remembered.
        if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true)
        {
            utils__storage_set("lemon_identity_string", g_identity_string);
        }
    }
    else if (e.data.type == "data_processing_worker__loopback_status")
    {
        // node still working keeps the spinner; a failed or idle state shows the page
        if (e.data.value.state == "connecting" || e.data.value.state == "connected")
        {
            connection__extend_connect_page_holdback();
        }
        else
        {
            connection__reveal_connect_page();
        }

        // node owns the real connection; its report replaces the local guesswork
        g_connection_status.state = e.data.value.state;
        g_connection_status.reason = e.data.value.reason;
        g_connection_status.next_retry_at = e.data.value.next_retry_at;
        g_connection_status.last_connected_at = e.data.value.last_connected_at;
        connection__render_connection_status();
    }
    else if (e.data.type == "data_processing_worker__authentication_status")
    {
        if (e.data.value == "success")
        {
            // a resume the server answered with a plain login: it had no session left to adopt, so
            // the page state is stale. one clean failure instead of a half-refreshed page
            if (g_fast_reconnect.in_progress == true && g_fast_reconnect.resumed == false)
            {
                connection__fast_reconnect_failed("the server had no session to resume");
                return;
            }

            g_connection_status.last_connected_at = new Date().valueOf();
            g_connection_status.next_retry_at = 0;
            connection__report_connection_status("connected");

            // node that logs in with nobody watching belongs in idle straight away,
            // otherwise it stands in the root channel looking present
            if (typeof android_host__node_apply_idle_for_ui_state === "function")
            {
                android_host__node_apply_idle_for_ui_state();
            }

            utils__hide_custom_alert(); // if there was any alert

            // the same policy set arrives again as "server_policy" whenever an admin saves,
            // so both paths share one application function
            server_settings_tab__apply_server_policy_fields(e.data.policy);

            // fresh session: the info card counts from this moment. a resumed one keeps counting
            if (g_fast_reconnect.resumed == false)
            {
                g_session.connected_at = new Date().valueOf();
                g_session.bytes_sent = 0;
                g_session.bytes_received = 0;
                g_session.last_ping_ms = -1;
            }

            // ask for the offline-people roster on every connect, from every device and theme.
            // the server decides: it answers only if it offers the list AND this user is
            // registered on it. a refusal is simply silence, so nothing here needs to know.
            client_msg.send_request_stored_clients();

            // the idle panel is wired once; a resume still has it (a second listener would cancel the first)
            if (e.data.is_idle_mode_allowed && g_fast_reconnect.resumed == false)
            {
                document.getElementById('channel-list-container').style.height = "calc(70% - 30px)";
                document.getElementById("idle-channel-collapse-button").addEventListener("mousedown", UI.collapse_expand_channel);
                document.getElementById('idle-clients-container').style.display = "block";
            }

            g_is_authenticated = true;
            console.log("client authenticated");

            // the attempt is over once the adopted session accepted the login; the refreshed
            // lists are still on their way and are applied together when the last one arrives
            if (g_fast_reconnect.resumed == true)
            {
                g_fast_reconnect.in_progress = false;
                console.log("fast reconnect: login accepted on the adopted session");
            }

            g_client_list = g_client_list;
            g_channel_list = g_channel_list;

            if (g_is_running_in_android_webview)
            {
                // the bar gear is the connected state's settings button; the connect screen has its
                // own, and tapping this one while connected asks first (it edits the live connection)
                document.getElementById("android-settings-button").style.display = "block";
            }

            // the connected ui is up, so the hold has served its purpose
            g_is_holding_back_connect_page = false;
            document.getElementById('verification-system').style.visibility = "";
            connection__set_connect_holdback_loader_visible(false);
            connection__set_connect_button_pending(false);

            document.getElementById('verification-system').style.display = "none";
            document.getElementById('communication-system-container').style.display = (g_layout.grid_active == true) ? "grid" : "block";

            // the connect screen is gone, so the mic may come back out
            voice__update_microphone_button();

            // people on touch device dont need to see chat by default, current UI isnt suitable for that
            // most likely they will just want to call with voice. a resume leaves the chat as the user had it
            if (g_is_client_running_under_touch_device && g_fast_reconnect.resumed == false)
            {
                UI.hide_chat_container();
            }

            if (!g_should_connection_check_be_running)
            {
                g_should_connection_check_be_running = true;
                connection__websocket_connection_check();
            }

            // a fresh session starts the server's datachannel attempt count from zero, so a retry loop
            // sitting out a cooldown may try again now (a resumed session keeps the server's cooldown)
            if (g_fast_reconnect.resumed == false)
            {
                g_webrtc_datachannel_cooldown_until_ms = 0;
            }
            if (g_datachannel_retry_sleep_resolve != null)
            {
                g_datachannel_retry_sleep_resolve();
            }

            // a resumed session was never gone from the user's point of view: no connected sound
            if (g_fast_reconnect.resumed == true)
            {
                console.log("fast reconnect: connected sound skipped");
            }
            else
            {
                // the connected sound waits a second: on android the client list that follows makes
                // java push the user settings (with the sound preference) right after this point
                setTimeout( () => {
                    if (g_are_sound_effects_enabled)
                    {
                        g_sound_effects.connected.play();
                    }
                }, 1000);
            }
        }
    }
    else if (e.data.type == "data_processing_worker__audio_enabled")
    {
        // default stun port is 3478 for this chat. Stun port is given to client with websocket connection, stun port can be changed if needed, there are two places that need to be edited in code of server to set different stun port

        g_iceconfig = {
            iceServers: [{
                urls: "stun:" + g_host + ":" + e.data.stun_port,
            }],
        };

        // the datachannel/playback below is set up whenever audio is on for clients or music bots,
        // so this flag (which keeps the datachannel established/reconnected) is always true here.
        // whether this client may transmit its own microphone is a separate flag, gated on client voice
        g_is_voice_chat_allowed_by_server = true;
        g_is_client_microphone_allowed_by_server = (e.data.client_voice_allowed == true);
        // from here on it is playback and webrtc; the headless service has no webaudio, no opus
        // decoder worker and no RTCPeerConnection, so it leaves before the constructor throws
        if (typeof window.AudioContext !== "function" && typeof window.webkitAudioContext !== "function")
        {
            console.log("no webaudio in this runtime, skipping playback and datachannel setup");
            return;
        }

        // decoded audio is 48 kHz pcm pushed in untouched, so the context is asked for 48 kHz and the
        // browser converts to the device rate; a 44.1 kHz context played it 9% slow and lagged more each second
        try
        {
            g_audio_context = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
        }
        catch (sample_rate_hint_not_supported)
        {
            g_audio_context = new (window.AudioContext || window.webkitAudioContext)();
        }
        console.log("audio_context.sampleRate" + g_audio_context.sampleRate);

        // without a user gesture the browser creates the context suspended and drops every frame;
        // resume now if a gesture already happened, else the first-gesture handler in main.js does
        if (g_audio_context.state === "suspended")
        {
            console.log("audio_context created suspended (autoplay policy, no user gesture yet); audio stays silent until a gesture resumes it");
            g_audio_context.resume();
        }

        let opus_decoding_sampler_channels = 1;
        let opus_decoding_sampler_input_rate = 48000;
        let opus_decoding_sampler_output_rate = g_audio_context.sampleRate;

        g_opus_decoding_sampler = new SpeexResampler(opus_decoding_sampler_channels,
            opus_decoding_sampler_input_rate,
            opus_decoding_sampler_output_rate);

        g_opus_decoder_worker.postMessage({
            type: "init",
            sampleRate: opus_decoding_sampler_output_rate
        });

        g_silence = new Float32Array(g_audio_config.codec.bufferSize);
        g_audio_player_gain_node = g_audio_context.createGain();
        g_audio_player_gain_node.connect(g_audio_context.destination);

        // AudioWorklet playback when the context supports it, ScriptProcessorNode otherwise. guarded,
        // because an exception here used to abort the handler before the webrtc datachannel check below
        try
        {
            audio__create_audio_player_output();
        }
        catch (audio_output_setup_error)
        {
            console.log("audio player output setup failed (" + audio_output_setup_error + "); continuing so the datachannel still gets created");
        }

        try
        {
            console.log("voice__create_new_peer_connection_object_for_use");

            // a re-login burst can arrive while a previous check loop is still sleeping;
            // that loop will pick the fresh config up itself, a second one just fights it
            if (g_is_webrtc_datachannel_check_running == false)
            {
                g_is_webrtc_datachannel_check_running = true;
                voice__webrtc_datachannel_connection_check(false);
            }
        }
        catch (Exception)
        {
            utils__custom_log(Exception.toString());
            utils__custom_alert('audio connection failed');
            return;
        }

        // android app went to background while the connect was still in progress - enter idle now
        if (g_is_deep_idle_pending == true)
        {
            g_is_deep_idle_pending = false;
            android_host__enter_deep_idle();
        }
    }
    else if (e.data.type == "data_processing_worker__metadata_keys_accepted")
    {
        g_keys_init_status = true;
    }
    else if (e.data.type == "data_processing_worker__client_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("client", e.data.value) == false)
        {
            server_msg.process_client_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__client_avatar_from_server")
    {
        server_msg.process_client_avatar_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__avatar_changed_from_server")
    {
        server_msg.process_avatar_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__connection_check_response")
    {
        g_connection_check.last_response_timestamp = new Date().valueOf();

        if (g_session.ping_sent_at > 0)
        {
            g_session.last_ping_ms = g_connection_check.last_response_timestamp - g_session.ping_sent_at;
        }
    }
    else if (e.data.type == "data_processing_worker__fast_reconnect_ok")
    {
        connection__fast_reconnect_succeeded();
    }
    else if (e.data.type == "data_processing_worker__datachannel_cooldown")
    {
        let seconds_left = parseInt(e.data.value.message.seconds_left);

        if (isNaN(seconds_left) == false && seconds_left > 0)
        {
            g_webrtc_datachannel_cooldown_until_ms = new Date().valueOf() + seconds_left * 1000;
            console.warn("datachannel: the server refuses new attempts for " + seconds_left + " s (10 attempts never connected)");
        }
    }
    else if (e.data.type == "data_processing_worker__channel_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("channel", e.data.value) == false)
        {
            server_msg.process_channel_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__tag_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("tag", e.data.value) == false)
        {
            server_msg.process_tag_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__identity_list_from_server")
    {
        server_msg.process_identity_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("icon", e.data.value) == false)
        {
            server_msg.process_icon_list_from_server(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__icon_add_from_server")
    {
        server_msg.process_icon_add_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_add_to_client_from_server")
    {
        server_msg.process_add_tag_to_client_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_alias_changed_from_server")
    {
        server_msg.process_client_alias_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_country_code_changed_from_server")
    {
        server_msg.process_client_country_code_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__typing_indicator_from_server")
    {
        chat__note_typing_from_client(e.data.value.message.client_id, e.data.value.message.receiver_type, e.data.value.message.receiver_id);
    }
    else if (e.data.type == "data_processing_worker__stored_clients_list_from_server")
    {
        server_msg.process_stored_clients_list_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__remove_tag_from_client_from_server")
    {
        server_msg.process_remove_tag_from_client_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_add_from_server")
    {
        server_msg.process_tag_add_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_delete_from_server")
    {
        server_msg.process_tag_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_delete_from_server")
    {
        server_msg.process_icon_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__tag_icon_changed_from_server")
    {
        server_msg.process_tag_icon_changed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_icon_changed_from_server")
    {
        server_msg.process_channel_icon_changed_from_server(e.data.value);
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

        let dh_public_mix = BigInt(dh_public_mix_string);

        // reject a degenerate server public mix (B <= 1 or B >= p-1) before using it; such values
        // force a known/tiny shared secret. for a safe prime, requiring 2 <= B <= p-2 is sufficient.
        if (dh_public_mix <= 1n || dh_public_mix >= g_dh_modulus - 1n)
        {
            console.error("rejected degenerate DH public mix from server; aborting handshake");
            return;
        }

        let shared_secret = lemon_crypto.modpow(dh_public_mix, g_dh_secret_exponent, g_dh_modulus);

        // add shared secret to

        // because there is also "secret key" used ,used to further secure connection between server and client, and is different for every connected client, (obtained with steps that wants to look like diffie-hellman key exchange)
        // this secret key needs to be added to metadata keys
        // and then copy of g_metadata_keys object sent to data processing webworker aswell (there are two copies of same object)

        let shared_secret_string = shared_secret.toString();

        // derive the AES enc key + HMAC mac key from the shared secret via HKDF (matches the server)
        let dh_keys = keys__dh_derive_keys(shared_secret_string);
        key_bytes = dh_keys.enc_key;

        let single_key = {
            info: "aes-ctr",
            key_string: shared_secret_string,
            key_bytes: key_bytes,
            mac_bytes: dh_keys.mac_key
        };

        g_metadata_keys.unshift(single_key);

        // removed: never log key material (g_metadata_keys holds the session key)

        // here the g_metadata_keys object within data_processing web worker is updated aswell

        console.log("public_key_challenge_response result -> ", msg.message.decryption_result);

        g_data_processing_worker.postMessage({
            type: "mainthread__metadata_keys",
            value: g_metadata_keys
        });

        let message_object = {
            message: {
                type: "public_key_challenge_response",
                value: msg.message.decryption_result,
                // asks the server to adopt the still-open session of this identity (fast reconnect)
                fast_reconnect: (g_fast_reconnect.in_progress == true)
            }
        };

        // the chosen username rides along on the last login message, because the server
        // assigns the final name right after accepting this response
        if (typeof g_chosen_username === "string" && g_chosen_username.length > 0)
        {
            message_object.message.chosen_username = g_chosen_username;
        }

        connection__send_message_object(message_object);
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
    else if (e.data.type == "data_processing_worker__force_admin_password_change")
    {
        // the admin password set during server setup was typed in cleartext; prompt once for a new one
        let new_admin_password = prompt("The admin password set at server setup was typed in cleartext in the console. Please set a new admin password now.");
        if (new_admin_password != null && new_admin_password != "")
        {
            let message_object = { message: { type: "change_admin_password", value: new_admin_password } };
            connection__send_message_object(message_object);
        }
    }
    else if (e.data.type == "data_processing_worker__client_info_from_server")
    {
        server_msg.process_client_info_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_full_from_server")
    {
        let full_channel = channel_tree__get_channel_by_id(g_channel_list, e.data.value.message.channel_id);
        let full_channel_name = (full_channel != null && full_channel.name != null) ? full_channel.name : "channel";
        utils__custom_alert("'" + full_channel_name + "' is full");
    }
    else if (e.data.type == "data_processing_worker__server_settings_values_from_server")
    {
        let msg = e.data.value;
        server_settings_tab__apply_server_settings_values_to_tab(msg.message);
        server_settings_tab__refresh_hide_admin_flag_row_visibility();
        chat_files__refresh_file_upload_size_visibility();
        chat_files__refresh_picture_size_visibility();
        server_settings_tab__set_blocked_countries_from_server(msg.message.blocked_countries);
        server_settings_tab__refresh_country_blocking_visibility();
        UI.render_bans_list(msg.message.bans);
    }
    else if (e.data.type == "data_processing_worker__admin_log_from_server")
    {
        server_msg.process_admin_log_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_policy_from_server")
    {
        server_settings_tab__apply_server_policy_fields(e.data.value.message);
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

        let client = {
            client_id: e.data.value.message.client_id,
            audio_state: e.data.value.message.value,
        }
        voice__process_audio_state_of_single_client(client);
    }
    else if (e.data.type == "data_processing_worker__current_channel_active_microphone_usage_from_server")
    {
        console.log("data_processing_worker__current_channel_active_microphone_usage_from_server");

        console.log(e.data.value);
        for (var i = 0; i < e.data.value.message.clients.length; i++)
        {
            voice__process_audio_state_of_single_client(e.data.value.message.clients[i]);
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
    else if (e.data.type == "data_processing_worker__channel_chat_file_metadata")
    {
        server_msg.process_channel_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_metadata")
    {
        server_msg.process_direct_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file")
    {
        server_msg.process_direct_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_file")
    {
        server_msg.process_channel_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_file_decrypt_failed")
    {
        server_msg.process_chat_file_decrypt_failed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__file_send_error_from_server")
    {
        server_msg.process_file_send_error_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__seen_receipt_message_result")
    {
        connection__websocket_worker_send(e.data.seen_receipt_message_content);
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
    else if (e.data.type == "data_processing_worker__offline_chat_message")
    {
        server_msg.process_offline_chat_message_from_server(e.data.value);
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
        connection__websocket_worker_send(e.data.channel_keys_message_content);
    }
    else if (e.data.type == "data_processing_worker__local_channel_keys_for_ui")
    {
        // node is the maintainer: it generated these keys itself, so no inbound frame
        // exists to forward. deliver our own copy to the attached ui directly
        if (g_node_frame_listener != null)
        {
            console.log("delivering locally generated channel keys to the ui");
            g_node_frame_listener(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__new_channel_keys_from_data_processing_worker")
    {
        g_current_channel_keys = e.data.value;

        // data processing worker created new channel keys and sent them to UI thread, now send them from UI thread to opus_decoder worker
        g_opus_decoder_worker.postMessage({
            type: "mainthread__channel_keys_for_opus_decoder",
            value: g_current_channel_keys
        });
    }
    else if (e.data.type == "data_processing_worker__tell_websocket_worker_to_send_data")
    {
        // the last stop before the socket: if a message reaches here and still does not
        // arrive, the fault is past the webview (node or the server), not in it
        utils__custom_log("[send-out] worker produced "
            + (e.data.data_to_be_sent_over_websocket || "").length + " chars for the socket");
        connection__websocket_worker_send(e.data.data_to_be_sent_over_websocket);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts")
    {
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        // the send path took the upload lock before handing the picture to the worker
        g_file_send_intent = "direct_chat_picture_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts")
    {
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        // the send path took the upload lock before handing the picture to the worker
        g_file_send_intent = "channel_chat_picture_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts"
        || e.data.type == "data_processing_worker__channel_chat_file_to_be_uploaded_by_parts")
    {
        // the send path took the upload lock before handing the file to the worker, so this
        // cannot collide with another upload the way a refused picture could
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_string_into_smaller_parts(e.data.data_for_upload_process, 400);

        g_file_send_intent = (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts") ? "direct_chat_file" : "channel_chat_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__chat_file_send_failed")
    {
        // the worker could not encrypt it (bad receiver key); the card and the lock are ours to clean up
        chat__release_file_upload_lock();
        utils__custom_alert("file not sent: " + e.data.reason);
        chat_files__mark_local_chat_file_card_failed(e.data.local_message_id, "not sent: " + e.data.reason);
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
