// routes worker replies to the shared client handlers
var dispatch_failed_deliveries = new Set();
var dispatch_accepted_deliveries = new Set();
var dispatch_processed_deliveries = new Set();
var dispatch_discarded_deliveries = new Set();

/**
 * @brief dispatches a current worker message and acknowledges accepted local delivery
 *
 * @param object e -> the worker message event
 *
 * @return void
 */
function dispatch__mainthread_onmessage(e)
{
    if (!e || !e.data)
    {
        return;
    }
    if (e.data.connection_generation != null && (e.data.connection_generation !== g_worker_connection_generation || (g_worker_connection_ended && e.data.type !== "websocket_worker_onclose" && e.data.type !== "websocket_worker_onerror")))
    {
        return;
    }
    // bind local delivery to the page's current session
    const completion = e.data.type === "data_processing_worker__local_delivery_accepted";
    const delivery = completion ? e.data.value : e.data.local_delivery;
    const key = delivery ? delivery.session_id + ":" + delivery.id : null;
    if (delivery && (!android_host__is_ui_only_runtime() || delivery.session_id !== node_session_id))
    {
        return;
    }
    if (completion)
    {
        // acknowledge only content the page successfully processed
        if (!dispatch_failed_deliveries.has(key) && (dispatch_processed_deliveries.has(key) || dispatch_accepted_deliveries.has(key)))
        {
            dispatch_accepted_deliveries.add(key);
            if (dispatch_accepted_deliveries.size > 4096)
            {
                dispatch_accepted_deliveries.delete(dispatch_accepted_deliveries.values().next().value);
            }
            connection__send_message_object(
            {
                message:
                {
                    type: "local_delivery_ack", id: delivery.id, session_id: delivery.session_id,
                    discard_attachment: dispatch_discarded_deliveries.has(key)
                }
            });
            // mark the visible conversation read after its content was accepted
            if (typeof chat__is_the_user_actually_looking === "function" && chat__is_the_user_actually_looking())
            {
                const channel = /^chat-context-channel-([0-9]+)$/.exec(g_current_chat_context_id);
                const peer = /^chat-context-pm-([0-9]+)$/.exec(g_current_chat_context_id);
                if (channel)
                {
                    chat__clear_channel_unread_count(Number(channel[1]));
                }
                else if (peer)
                {
                    chat__clear_unread_count(Number(peer[1]));
                }
                else if (g_current_chat_context_id.indexOf("chat-context-offline-") === 0)
                {
                    const context = g_chat_context_array.find(function(candidate)
                    {
                        return candidate.chat_context_id === g_current_chat_context_id;
                    });
                    const identity_hash = context && context.peer_identity_hash;
                    const public_key = context && context.peer_public_key;
                    if (identity_hash || public_key)
                    {
                        chat__send_read_command(identity_hash ? "identity:" + identity_hash : "key:" + public_key,
                        {
                            identity_hash: identity_hash || "",
                            public_key: public_key || ""
                        });
                    }
                }
            }
        }
        dispatch_failed_deliveries.delete(key);
        dispatch_processed_deliveries.delete(key);
        dispatch_discarded_deliveries.delete(key);
        return;
    }
    // ignore content already accepted before this replay
    if (key && dispatch_accepted_deliveries.has(key))
    {
        return;
    }
    try
    {
        const result = dispatch__mainthread_onmessage_impl(e);
        if (key)
        {
            if (result === false)
            {
                dispatch_processed_deliveries.delete(key);
                dispatch_failed_deliveries.add(key);
                connection__send_message_object({message:
                {
                    type: "local_delivery_ack", id: delivery.id, session_id: delivery.session_id,
                    accepted: false
                }});
                return;
            }
            if (result === "deferred")
            {
                return;
            }
            // attachment discard also cancels its retained body
            if (result === "discarded" && /^data_processing_worker__(direct|channel)_chat_(file|picture)_metadata$/.test(e.data.type))
            {
                dispatch_discarded_deliveries.add(key);
            }
            dispatch_processed_deliveries.add(key);
            const context_id = chat__delivery_context(e.data.value);
            if (context_id)
            {
                chat_last_delivery_by_context.set(context_id, delivery.id);
            }
        }
    }
    catch (error)
    {
        if (key)
        {
            dispatch_failed_deliveries.add(key);
        }
        throw error;
    }
}

/**
 * @brief routes a worker event to its connection, chat or media handler
 *
 * @param object e -> the worker message event
 *
 * @return boolean|string|undefined the handler's delivery result
 */
function dispatch__mainthread_onmessage_impl(e)
{
    if (e.data.connection_generation != null && (e.data.connection_generation !== g_worker_connection_generation || (g_worker_connection_ended && e.data.type !== "websocket_worker_onclose" && e.data.type !== "websocket_worker_onerror")))
    {
        return;
    }
    if (DBG_WORKER_BOOT_LOG)
    {
        console.log("[m<-w] " + (e.data && e.data.type));
    }

    if (e.data.type == "log")
    {
        utils__custom_log(e.data.value);
    }
    else if (typeof e.data.type === "string" && e.data.type.indexOf("video_worker__") == 0)
    {
        // everything the video worker posts back: packets to send, keyframe requests, decoder state
        video__on_worker_message(e.data);
    }
    else if (e.data.type == "opus_encoder_worker__encode_result")
    {
        // discard encoder output queued before the microphone stopped
        if (g_is_microphone_active == false && (typeof is_playing_music == "undefined" || is_playing_music != true))
        {
            return;
        }

        if (g_current_channel_keys != null)
        {
            let opus_data_chunks = e.data.value;

            for (var i = 0; i < opus_data_chunks.length; i++)
            {
                // put the sequence number inside encryption so receivers can reorder frames privately
                let opus_chunk_bytes = new Uint8Array(opus_data_chunks[i]);
                let sequenced_chunk = new Uint8Array(2 + opus_chunk_bytes.length);

                sequenced_chunk[0] = g_voice_send_sequence_number & 0xff;
                sequenced_chunk[1] = (g_voice_send_sequence_number >> 8) & 0xff;
                sequenced_chunk.set(opus_chunk_bytes, 2);
                g_voice_send_sequence_number = (g_voice_send_sequence_number + 1) & 0xffff;

                // the encryption layers and the sending are in audio-transport.js
                audio_transport__send_voice(sequenced_chunk);
            }
        }
        else
        {
            utils__custom_log("dont have channel keys, cant encrypt audio for sending");
        }
    }
    else if (e.data.type == "opus_encoder_worker__stream_encode_result")
    {
        // the sound of this client's video stream: packed and sent next to the pictures by video.js, never as voice
        if (typeof video__on_sound_encoded === "function")
        {
            video__on_sound_encoded(e.data.value);
        }
    }
    else if (e.data.type == "opus_decoder_worker__decode_result")
    {
        audio__audio_player_write_chunk(e.data.value);
    }
    else if (e.data.type == "minimp3_worker__decode_result")
    {
        if (!groups__can_send_to_channel("talk", g_current_channel_id))
        {
            groups__notify_channel_power_denied("talk");
            is_playing_music = false;
            g_is_microphone_active = false;
            g_last_sent_value_microphone_usage = 2;
            if (G_HAS_DOM)
            {
                document.getElementById("custom-file-upload-button-song").style.display = "inline-block";
                document.getElementById("stop-song").style.display = "none";
            }
            return;
        }
        let message_object1 = {
            message:
            {
                type: "start_song_stream",
                song_name: g_selected_song_name
            }
        };

        connection__send_message_object(message_object1);

        voice__stream_local_mp3_file_to_other_clients(e.data.value, e.data.mp3_sample_rate);

        if (g_alert_streaming_music_shown_once == false)
        {
            utils__custom_alert("when streaming music from file, detach browser tab where chat is to separate window (if using multiple tabs) or stay focused on tab where chat is otherwise music is not sent reliably and it lags");
            g_alert_streaming_music_shown_once = true;
        }
    }
    else if (e.data.type == "websocket_worker_onmessage")
    {
        if (e.data.value != null && e.data.value.length > 0)
        {
            g_session.bytes_received += e.data.value.length;
        }

        // forward socket worker frames to the data worker
        g_data_processing_worker.postMessage({
            type: "mainthread__process_received_websocket_message",
            value: e.data.value,
            connection_generation: e.data.connection_generation,
            // loopback frames arrive as plain json, the worker skips decryption
            is_plaintext: android_host__is_ui_only_runtime()
        });
    }
    else if (e.data.type == "data_processing_worker__rsa_key_too_weak")
    {
        keys__handle_rsa_key_too_weak_notice(e.data.minimum_rsa_key_bits);
    }
    else if (e.data.type == "data_processing_worker__connection_activity")
    {
        g_connection_check.last_response_timestamp = new Date().valueOf();
    }
    else if (e.data.type == "websocket_worker_onclose")
    {
        if (android_host__is_ui_only_runtime())
        {
            connection_loopback_ready = false;
        }
        connection__play_disconnect_sound(e.data.close_code);
        if (e.data.reason)
        {
            g_last_disconnect_reason = e.data.reason;
        }
        if (!android_host__is_ui_only_runtime() && (e.data.close_code === G_SERVER_CLOSE_KICKED || e.data.close_code === G_SERVER_CLOSE_BANNED))
        {
            g_last_disconnect_reason = e.data.close_code === G_SERVER_CLOSE_KICKED ? "you were kicked from the server" : "you were banned from the server";
            // a moderator ended this connection. wait for an explicit connect request
            is_connection_ended_by_user = true;
            is_connect_loop_wake_pending = false;
            g_fast_reconnect.in_progress = false;
        }
        utils__custom_log("WebSocket closed: code=" + e.data.close_code + ", clean=" + e.data.was_clean + (e.data.close_reason ? ", reason=" + e.data.close_reason : ""));
        audio_transport__reset();
        // let the connection loop decide whether to reconnect
        g_last_connect_attempt_failed = true;

        // without an explicit server reason, infer the cause from where the socket died
        // a local page socket closing does not describe the remote connection. Node reports that state
        if (android_host__is_ui_only_runtime() == false)
        {
            if (g_is_authenticated == true)
            {
                g_connection_status.last_connected_at = new Date().valueOf();

                if (g_last_disconnect_reason === "")
                {
                    g_last_disconnect_reason = "connection closed (WebSocket code " + (e.data.close_code || "unknown") + ")";
                }
            }
            else if (g_last_disconnect_reason === "")
            {
                g_last_disconnect_reason = (g_device_has_network === false) ? "no network connection (wifi and mobile data are off)" : "the server probably rejected the keys (it closed the connection during login)";
            }
        }

        connection__signal_connection_closed();
        console.warn("server connection lost (identity switch in progress: " + g_is_identity_switch_in_progress + ")");

        // retry within the original recovery deadline and require a valid login on every attempt
        if (g_fast_reconnect.in_progress == true)
        {
            console.warn("resume socket closed; retrying within the recovery deadline");
        }
        else if (connection__try_start_fast_reconnect("socket closed") == false)
        {
            // do not show connection loss for an intentional close
            let is_connection_lost = (g_is_authenticated && g_is_identity_switch_in_progress == false && is_connection_ended_by_user == false);
            if (is_connection_lost && g_is_running_in_android_webview == false)
            {
                utils__custom_alert("connection with server was lost");
            }
            g_is_authenticated = false;

            connection__reset_chat_app_keep_identity();

            // after a lost connection the connection details are shown, so the user sees the server and can connect again
            if (is_connection_lost && typeof connection__show_connect_window === "function")
            {
                // a menu that was open belongs to the connection that ended
                UI.delete_contextmenus(true);
                connection__show_connect_window();
            }
        }
    }
    else if (e.data.type == "websocket_worker_onerror")
    {
        connection__play_disconnect_sound();
        audio_transport__reset();
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
            console.warn("resume socket failed; retrying within the recovery deadline");
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
    else if (e.data.type == "data_processing_worker__generate_rsa_keypair_failed")
    {
        if (connection__accept_identity_result(e.data))
        {
            connection__identity_generation_failed();
            if (g_node_frame_listener != null)
            {
                g_node_frame_listener(JSON.stringify({message:
                {
                    type: "local_identity_result", identity_revision: e.data.identity_revision,
                    rsa_key_bits: e.data.rsa_key_bits, success: false, public_key_string: ""
                }}));
            }
        }
    }
    else if (e.data.type == "data_processing_worker__generate_rsa_keypair_result")
    {
        if (!connection__accept_identity_result(e.data))
        {
            return;
        }
        g_rsa_public_key_string = e.data.value;
        g_is_rsa_key_generated = true;

        // clear the completed key generation phase before the next connection attempt
        if (g_is_authenticated == false && g_is_autoconnect_without_user_action_active == false)
        {
            connection__report_connection_status("idle", "");
        }
        document.getElementById("another-buttons-sub-loading-container").style.display = "none";
        document.getElementById("another-buttons-sub-container").style.display = "";
        g_identity_string = e.data.identity_string;
        if (g_node_frame_listener != null)
        {
            g_node_frame_listener(JSON.stringify({message:
            {
                type: "local_identity_result", identity_revision: g_identity_revision,
                rsa_key_bits: g_identity_rsa_key_bits, success: true, public_key_string: g_rsa_public_key_string
            }}));
        }

        // wakes the connect loop if it is waiting to connect with this identity
        g_identity_slot.set({ public_key_string: e.data.value, identity_string: e.data.identity_string });

        // the identity section of the connect window shows the new key pair's fingerprint
        if (typeof connection__refresh_connect_window_sections === "function")
        {
            connection__refresh_connect_window_sections();
        }
        g_is_identity_switch_in_progress = false;

        // this key pair was made from an identity the user imported: tell the user that the import finished
        if (g_is_identity_import_pending == true)
        {
            connection__identity_imported();
        }

        // save the selected passphrase so the next launch restores this identity
        if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true)
        {
            utils__storage_set("lemon_identity_string", g_identity_string);
            utils__storage_set("lemon_identity_record_v1", JSON.stringify(
            {
                passphrase: g_identity_string,
                rsa_key_bits: g_identity_rsa_key_bits
            }));
        }
    }
    else if (e.data.type == "data_processing_worker__loopback_status")
    {
        const state = e.data.value;
        if (state == null || typeof state.state !== "string")
        {
            return;
        }
        if (android_host__is_ui_only_runtime())
        {
            connection_loopback_ready = true;
            connection__flush_local_connect_intent();
            if ((state.state === "idle" || state.state === "error") && (g_is_authenticated || g_is_client_list_retrieved || node_session_id))
            {
                connection__reset_chat_app_keep_identity(true);
                node_session_id = "";
            }
            else if (state.state === "connecting")
            {
                g_is_authenticated = false;
            }
        }
        // keep the spinner while Node is working. reveal the page when it stops
        if (e.data.value.state == "connecting" || e.data.value.state == "connected")
        {
            connection__extend_connect_page_holdback();
        }
        else
        {
            connection__reveal_connect_page();
        }

        // use Node's report for the remote connection state
        g_connection_status.state = e.data.value.state;
        g_connection_status.reason = e.data.value.reason;
        g_connection_status.next_retry_at = e.data.value.next_retry_at;
        g_connection_status.last_connected_at = e.data.value.last_connected_at;
        connection__render_connection_status();
    }
    else if (e.data.type == "data_processing_worker__local_transfer_ack")
    {
        connection__send_message_object(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__local_upload_result")
    {
        const finish = connection_local_uploads.get(e.data.value.request_id);
        if (finish != null)
        {
            finish(e.data.value.accepted === true);
        }
    }
    else if (e.data.type == "data_processing_worker__local_session_state")
    {
        const state = e.data.value;
        if (!android_host__is_ui_only_runtime() || typeof state.public_key_string !== "string" || !Number.isSafeInteger(state.identity_revision) || state.identity_revision < g_identity_revision)
        {
            return;
        }
        if (node_session_id && node_session_id !== state.session_id)
        {
            connection__reset_chat_app_keep_identity(true);
        }
        g_rsa_public_key_string = state.public_key_string;
        g_identity_revision = state.identity_revision;
        node_session_id = state.session_id;
        g_is_rsa_key_generated = true;
        chat__hydrate_peer_states(state.peer_states);
        chat__hydrate_channel_counts(state.channel_counts);
    }
    else if (e.data.type == "data_processing_worker__local_peer_state")
    {
        const state = e.data.value;
        if (!android_host__is_ui_only_runtime() || state.session_id !== node_session_id)
        {
            return;
        }
        chat__hydrate_peer_states(state.peer_states);
        chat__hydrate_channel_counts(state.channel_counts);
        chat__restore_peer_state();
        for (const client of g_client_list)
        {
            chat__render_unread_badge(client.client_id, client.unread_count > 0);
        }
        UI.schedule_member_list_sync();
    }
    else if (e.data.type == "data_processing_worker__local_identity_result")
    {
        const result = e.data.value;
        if (!android_host__is_ui_only_runtime() || !Number.isSafeInteger(result.identity_revision) || result.identity_revision < g_identity_revision)
        {
            return;
        }
        g_identity_revision = result.identity_revision;
        g_is_rsa_key_generated = result.success === true;
        if (result.success === true)
        {
            g_rsa_public_key_string = result.public_key_string;
            g_identity_rsa_key_bits = result.rsa_key_bits;
        }
        if (g_is_identity_import_pending && result.identity_revision === g_identity_import_expected_revision)
        {
            g_identity_import_expected_revision = 0;
            if (result.success === true)
            {
                document.getElementById("another-buttons-sub-loading-container").style.display = "none";
                document.getElementById("another-buttons-sub-container").style.display = "";
                connection__identity_imported();
            }
            else
            {
                connection__identity_generation_failed();
            }
        }
    }
    else if (e.data.type == "data_processing_worker__authentication_status")
    {
        if (e.data.value == "success")
        {
            // clear the expired session and retry with a fresh login
            if (g_fast_reconnect.in_progress == true && g_fast_reconnect.resumed == false)
            {
                utils__custom_log("old session expired; reconnecting with a fresh session");
                g_is_authenticated = false;
                connection__reset_chat_app_keep_identity();
                connection__wake_connect_loop();
                return;
            }

            g_connection_status.last_connected_at = new Date().valueOf();
            g_connection_status.next_retry_at = 0;
            connection__report_connection_status("connected");

            // wait for refreshed lists before dismissing this resume notice
            if (g_fast_reconnect.resumed == false)
            {
                utils__hide_custom_alert();
            }

            // apply login policy and later server_policy updates through the same handler
            server_settings_tab__apply_server_policy_fields(e.data.policy);

            // fresh session: the info card counts from this moment. a resumed one keeps counting
            if (g_fast_reconnect.resumed == false)
            {
                if (!android_host__is_ui_only_runtime())
                {
                    android_host__begin_session();
                }
                g_session.connected_at = new Date().valueOf();
                g_session.bytes_sent = 0;
                g_session.bytes_received = 0;
                g_session.last_ping_ms = -1;
            }

            // wait for the client and group lists before checking access to stored users
            groups_window.stored_users_allowed = null;
            groups_window.access = null;

            // bind the idle handler once, including across resumed logins
            if (e.data.is_idle_mode_allowed && g_fast_reconnect.resumed == false)
            {
                document.getElementById('channel-list-container').style.height = "calc(70% - 30px)";
                document.getElementById("idle-channel-collapse-button").addEventListener("mousedown", UI.collapse_expand_channel);
                document.getElementById('idle-clients-container').style.display = "block";
            }

            g_is_authenticated = true;
            // presence changes require the newly accepted login to be active first
            if (typeof android_host__node_apply_idle_for_ui_state === "function")
            {
                android_host__node_apply_idle_for_ui_state();
            }
            groups__apply_server_mode();
            server_emojis__connected();
            chat_files__refresh_receive_preferences_controls();
            chat_files__send_receive_preferences();
            // close the previous audio socket after applying the new policy
            audio_transport__reset();
            console.log("client authenticated");

            // login finishes the attempt. the last refreshed list completes recovery
            if (g_fast_reconnect.resumed == true)
            {
                g_fast_reconnect.in_progress = false;
                console.log("fast reconnect: login accepted on the adopted session");
            }

            g_client_list = g_client_list;
            g_channel_list = g_channel_list;

            if (g_is_running_in_android_webview)
            {
                // connected settings require confirmation before editing the live connection
                document.getElementById("android-settings-button").style.display = "block";
            }

            // the connected ui is up, so the hold has served its purpose
            g_is_holding_back_connect_page = false;
            document.getElementById('verification-system').style.visibility = "";
            connection__set_connect_holdback_loader_visible(false);
            connection__set_connect_button_pending(false);

            document.getElementById('verification-system').style.display = "none";
            document.getElementById('communication-system-container').style.display = (g_layout.grid_active == true) ? "grid" : "block";
            layout__layout_apply();
            connection__mark_connected_ui_hidden(false);

            // refresh the browser connection menu after login
            if (typeof head_menu__close_all === "function")
            {
                head_menu__close_all();
                head_menu__refresh_connections_items();
            }
            connection__refresh_connect_button_state();

            // show connection fields when the dialog is opened after login
            document.getElementById("another-buttons-sub-loading-container").style.display = "none";
            document.getElementById("another-buttons-sub-container").style.display = "";

            // the connect screen is gone, so the mic may come back out
            voice__update_microphone_button();

            // open People on a fresh touch session. preserve the selected page on resume
            if (g_is_client_running_under_touch_device && g_fast_reconnect.resumed == false)
            {
                // theme opens chat
                let is_chat_shown_after_login = getComputedStyle(document.documentElement).getPropertyValue("--chat-shown-after-login").trim() === "1";
                if (!is_chat_shown_after_login)
                {
                    UI.hide_chat_container();
                }
            }

            if (!g_worker_connection_active && !g_should_connection_check_be_running)
            {
                g_should_connection_check_be_running = true;
                connection__websocket_connection_check();
            }

            // reset the data channel retry count only for a fresh session
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
                // allow native sound settings to arrive before playing the connected sound
                setTimeout( () => {
                    if (g_are_sound_effects_enabled)
                    {
                        sounds__play("connected");
                    }
                }, 1000);
            }
        }
    }
    else if (e.data.type == "data_processing_worker__audio_enabled")
    {
        // use the STUN port announced by the server

        g_iceconfig = {
            iceServers: Number(e.data.stun_port) > 0 ? [{
                urls: "stun:" + g_host + ":" + e.data.stun_port,
            }] : [],
        };

        // maintain the data channel for voice or bot playback. microphone permission is separate
        g_is_voice_chat_allowed_by_server = true;
        g_is_client_microphone_allowed_by_server = (e.data.client_voice_allowed == true);
        // Node has no browser audio or WebRTC objects
        if (typeof window.AudioContext !== "function" && typeof window.webkitAudioContext !== "function")
        {
            console.log("no webaudio in this runtime, skipping playback and datachannel setup");
            return;
        }

        // Opus produces 48 kHz PCM. let the browser convert it to the device rate
        try
        {
            g_audio_context = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
        }
        catch (sample_rate_hint_not_supported)
        {
            g_audio_context = new (window.AudioContext || window.webkitAudioContext)();
        }
        console.log("audio_context.sampleRate" + g_audio_context.sampleRate);

        // resume after a user gesture or wait for the gesture handler in main.js
        if (g_audio_context.state === "suspended")
        {
            console.log("audio_context created suspended (autoplay policy, no user gesture yet); audio stays silent until a gesture resumes it");
            g_audio_context.resume();
        }

        let opus_decoding_sampler_channels = 1;
        let opus_decoding_sampler_input_rate = 48000;
        let opus_decoding_sampler_output_rate = g_audio_context.sampleRate;

        g_opus_decoding_sampler = new SpeexResampler(opus_decoding_sampler_channels, opus_decoding_sampler_input_rate, opus_decoding_sampler_output_rate);

        g_opus_decoder_worker.postMessage({
            type: "init",
            sampleRate: opus_decoding_sampler_output_rate
        });

        g_silence = new Float32Array(g_audio_config.codec.bufferSize);
        g_audio_player_gain_node = g_audio_context.createGain();
        g_audio_player_gain_node.connect(g_audio_context.destination);

        // use AudioWorklet when supported and retain fallback playback if setup fails
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

            // the existing check loop uses the new settings. do not start a second loop
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

        // apply idle requested while the connection was starting
        if (g_is_deep_idle_pending == true)
        {
            android_host__apply_pending_deep_idle_if_any();
        }
    }
    else if (e.data.type == "data_processing_worker__music_bot_key")
    {
        audio_transport__set_music_bot_key(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__audio_websocket_session")
    {
        audio_transport__session(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__video_websocket_session")
    {
        video_transport__session(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__local_media_keys")
    {
        // android webview only: the node runtime handed over the complete media key set
        keys__set_media_keys_from_host(e.data.value.encryption_key, e.data.value.checksum_key, e.data.value.server_keys, e.data.value.session);
        video__post_transport_keys_to_worker();
    }
    else if (e.data.type == "data_processing_worker__metadata_keys_accepted")
    {
        g_keys_init_status = true;
    }
    else if (e.data.type == "data_processing_worker__client_list_from_server")
    {
        if (typeof server_info__clients_changed === "function")
        {
            server_info__clients_changed();
        }
        if (connection__fast_reconnect_buffer_list("client", e.data.value) == false)
        {
            channel_tree__apply_server_update(server_msg.process_client_list_from_server, e.data.value);
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
    else if (e.data.type == "data_processing_worker__connection_bytes")
    {
        g_session.bytes_sent += e.data.bytes_sent;
        g_session.bytes_received += e.data.bytes_received;
    }
    else if (e.data.type == "data_processing_worker__connection_check_response")
    {
        g_connection_check.last_response_timestamp = e.data.received_at || Date.now();

        if (typeof e.data.ping_ms === "number")
        {
            g_session.last_ping_ms = e.data.ping_ms;
        }
        else if (g_session.ping_sent_at > 0)
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
            channel_tree__apply_server_update(server_msg.process_channel_list_from_server, e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__moderation")
    {
        moderation__receive(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_emoji")
    {
        server_settings_tab__encryption_save_result(e.data.value);
        server_emojis__receive(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__group_list_from_server")
    {
        if (connection__fast_reconnect_buffer_list("group", e.data.value) == false)
        {
            server_msg.process_group_list_from_server(e.data.value);
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
    else if (e.data.type == "data_processing_worker__group_add_to_client_from_server")
    {
        server_msg.process_add_group_to_client_from_server(e.data.value);
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
    else if (e.data.type == "data_processing_worker__remove_group_from_client_from_server")
    {
        server_msg.process_remove_group_from_client_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__group_permissions_changed")
    {
        server_msg.process_group_permissions_changed(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__group_add_from_server")
    {
        server_msg.process_group_add_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__group_delete_from_server")
    {
        server_msg.process_group_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__icon_delete_from_server")
    {
        server_msg.process_icon_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__group_icon_changed_from_server")
    {
        server_msg.process_group_icon_changed_from_server(e.data.value);
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
    else if (e.data.type == "data_processing_worker__video_stream_state_from_server")
    {
        server_msg.process_video_stream_state_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__video_stream_offer_from_server")
    {
        server_msg.process_video_stream_offer_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__video_stream_viewer_request_from_server")
    {
        server_msg.process_video_stream_viewer_request_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__video_stream_viewer_state_from_server")
    {
        server_msg.process_video_stream_viewer_state_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__video_stream_keyframe_request_from_server")
    {
        server_msg.process_video_stream_keyframe_request_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__video_stream_refused_from_server")
    {
        server_msg.process_video_stream_refused_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_message_delete_from_server")
    {
        server_msg.process_chat_message_delete_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__crypto_policy")
    {
        try
        {
            connection__send_message_object(connection_crypto__accept_policy(e.data.value));
        }
        catch (error)
        {
            console.error(error.message);
            connection__disconnect_by_user();
        }
    }
    else if (e.data.type == "data_processing_worker__public_key_challenge_from_server")
    {
        let msg = e.data.value;
        try
        {
            g_metadata_keys.unshift(...connection_crypto__finish(msg.message));
        }
        catch (error)
        {
            console.error(error.message);
            connection__disconnect_by_user();
            return;
        }
        let media_keys = keys__media_session_layer();

        // Node gives the local page the connection keys needed for audio and video
        // this callback never sends those keys to the remote server
        if (android_host__is_ui_only_runtime() == false && typeof g_node_frame_listener !== "undefined" && g_node_frame_listener != null)
        {
            try
            {
                g_node_frame_listener(JSON.stringify(
                {
                    message:
                    {
                        type: "local_media_keys",
                        encryption_key: keys__bytes_to_hex(media_keys.key_bytes),
                        session: media_keys,
                        checksum_key: keys__bytes_to_hex(media_keys.mac_bytes),
                        server_keys: keys__media_server_key_layers().map(function(layer)
                        {
                            return { type: layer.key_type || "aes256", key: keys__bytes_to_hex(layer.key_bytes) };
                        })
                    }
                }));
            }
            catch (listener_error)
            {
                console.log("the media keys could not be handed to the ui: " + listener_error.message);
            }
        }

        // keep connection keys out of logs

        // update the data worker's copy of the connection keys



        let message_object = {
            message: {
                type: "public_key_challenge_response",
                value: msg.message.decryption_result,
                // ask the server to resume this identity's existing session
                fast_reconnect: (g_fast_reconnect.in_progress == true)
            }
        };

        // send the requested username before the server assigns the final login name
        if (typeof g_chosen_username === "string" && g_chosen_username.length > 0)
        {
            message_object.message.chosen_username = g_chosen_username;
        }

        if (g_worker_connection_active)
        {
            let response = keys__encrypt_all_message_data_and_convert_to_base64(connection__process_message_before_sending(message_object));
            g_session.bytes_sent += response.length;
            g_data_processing_worker.postMessage(
            {
                type: "mainthread__metadata_keys",
                value: g_metadata_keys,
                connection_generation: g_worker_connection_generation,
                send_after_update: response
            });
        }
        else
        {
            g_data_processing_worker.postMessage(
            {
                type: "mainthread__metadata_keys",
                value: g_metadata_keys
            });
            connection__send_message_object(message_object);
        }
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
    else if (e.data.type == "data_processing_worker__channel_description_from_server")
    {
        server_msg.process_channel_description_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__force_admin_password_change")
    {
        // prompt to replace the initial administrator password
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
    else if (e.data.type == "data_processing_worker__server_info")
    {
        if (typeof server_info__apply === "function")
        {
            server_info__apply(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__server_banner_result")
    {
        if (typeof server_info__banner_result === "function")
        {
            server_info__banner_result(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__server_settings_values_from_server")
    {
        if (!G_HAS_DOM)
        {
            return;
        }
        let msg = e.data.value;
        if (msg.message.memory_only === true)
        {
            server_info__apply_admin_memory(msg.message);
            return;
        }
        server_settings_tab__apply_server_settings_values_to_tab(msg.message);
        server_settings_tab__refresh_hide_admin_flag_row_visibility();
        server_settings_tab__refresh_audio_connections_row_visibility();
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
    else if (e.data.type == "data_processing_worker__url_resolved")
    {
        link_previews__resolved(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__admin_status")
    {
        groups__apply_admin_status(e.data.value.is_admin);
        server_emojis__render_admin();
    }
    else if (e.data.type == "data_processing_worker__server_policy_from_server")
    {
        server_settings_tab__apply_server_policy_fields(e.data.value.message);
    }
    else if (e.data.type == "data_processing_worker__channel_join_from_server")
    {
        channel_tree__apply_server_update(server_msg.process_channel_join_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__server_chat_message_id_for_local_message_id_from_server")
    {
        server_msg.process_server_chat_message_id_for_local_message_id_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_connect_from_server")
    {
        if (typeof server_info__clients_changed === "function")
        {
            server_info__clients_changed();
        }
        channel_tree__apply_server_update(server_msg.process_client_connect_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_rename_from_server")
    {
        channel_tree__apply_server_update(server_msg.process_client_rename_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_disconnect_from_server")
    {
        if (typeof server_info__clients_changed === "function")
        {
            server_info__clients_changed();
        }
        channel_tree__apply_server_update(server_msg.process_client_disconnect_from_server, e.data.value);
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
        channel_tree__apply_server_update(server_msg.process_channel_edit_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_going_to_idle_mode")
    {
        channel_tree__apply_server_update(server_msg.process_client_client_going_to_idle_mode_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__client_coming_back_from_idle_mode")
    {
        channel_tree__apply_server_update(server_msg.process_client_coming_back_from_idle_mode_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_delete_from_server")
    {
        channel_tree__apply_server_update(server_msg.process_channel_delete_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_create_from_server")
    {
        channel_tree__apply_server_update(server_msg.process_channel_create_from_server, e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture_metadata")
    {
        return server_msg.process_channel_chat_picture_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture_metadata")
    {
        return server_msg.process_direct_chat_picture_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_file_metadata")
    {
        return server_msg.process_channel_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_metadata")
    {
        return server_msg.process_direct_chat_file_metadata_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file")
    {
        return server_msg.process_direct_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_file")
    {
        return server_msg.process_channel_chat_file_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_file_decrypt_failed")
    {
        return server_msg.process_chat_file_decrypt_failed_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__chat_picture_decrypt_failed")
    {
        return server_msg.process_chat_picture_decrypt_failed_from_server(e.data.value);
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
        return server_msg.process_channel_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_message")
    {
        return server_msg.process_direct_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__offline_chat_message")
    {
        return server_msg.process_offline_chat_message_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture")
    {
        return server_msg.process_direct_chat_picture_from_server(e.data.value);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture")
    {
        return server_msg.process_channel_chat_picture_from_server(e.data.value);
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
        if (!keys__is_channel_key_creation_current(e.data))
        {
            return;
        }
        console.log("sending channel keys to user -> " + e.data.username);
        connection__websocket_worker_send(e.data.channel_keys_message_content);
    }
    else if (e.data.type == "data_processing_worker__local_channel_keys_for_ui")
    {
        if (!keys__is_channel_key_creation_current(e.data))
        {
            return;
        }
        // when Node maintains the channel, deliver its generated keys directly to the page
        if (g_node_frame_listener != null)
        {
            console.log("delivering locally generated channel keys to the ui");
            g_node_frame_listener(e.data.value);
        }
    }
    else if (e.data.type == "data_processing_worker__channel_key_creation_failed")
    {
        if (keys__is_channel_key_creation_current(e.data))
        {
            keys__finish_channel_key_creation();
        }
    }
    else if (e.data.type == "data_processing_worker__new_channel_keys_from_data_processing_worker")
    {
        if (!keys__is_channel_key_creation_current(e.data))
        {
            return;
        }
        if (e.data.reused_keys === true && g_current_channel_keys != null)
        {
            keys__finish_channel_key_creation();
            return;
        }
        keys__set_current_channel_keys(e.data.value);

        // install the channel keys in the audio decoder
        g_opus_decoder_worker.postMessage({
            type: "mainthread__channel_keys_for_opus_decoder",
            value: g_current_channel_keys
        });

        // the video worker encrypts and decrypts stream packets with the same keys
        video__post_channel_keys_to_worker();
        keys__finish_channel_key_creation();
    }
    else if (e.data.type == "data_processing_worker__tell_websocket_worker_to_send_data")
    {
        // trace the worker result before handing it to the socket
        utils__custom_log("[send-out] worker produced " + (e.data.data_to_be_sent_over_websocket || "").length + " chars for the socket");
        connection__websocket_worker_send(e.data.data_to_be_sent_over_websocket);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_picture_to_be_uploaded_by_parts")
    {
        if (!chat__picture_preparation_succeeded(e.data))
        {
            return;
        }
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_upload_into_chunks(e.data.data_for_upload_process);

        // the send path took the upload lock before handing the picture to the worker
        g_file_send_intent = "direct_chat_picture_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__channel_chat_picture_to_be_uploaded_by_parts")
    {
        if (!chat__picture_preparation_succeeded(e.data))
        {
            return;
        }
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_upload_into_chunks(e.data.data_for_upload_process);

        // the send path took the upload lock before handing the picture to the worker
        g_file_send_intent = "channel_chat_picture_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts" || e.data.type == "data_processing_worker__channel_chat_file_to_be_uploaded_by_parts")
    {
        // the send path already holds the upload lock for this file
        let total_bytes_length = e.data.data_for_upload_process.length;
        let parts = chat__split_upload_into_chunks(e.data.data_for_upload_process);

        g_file_send_intent = (e.data.type == "data_processing_worker__direct_chat_file_to_be_uploaded_by_parts") ? "direct_chat_file" : "channel_chat_file";
        g_file_send_intent_extra_data = e.data.extra_data;
        g_is_file_being_uploaded = false;
        chat__send_file_by_parts(parts, total_bytes_length, 5);
    }
    else if (e.data.type == "data_processing_worker__chat_file_send_failed")
    {
        // release the upload lock and mark the card after encryption fails
        chat__release_file_upload_lock();
        utils__custom_alert("file not sent: " + e.data.reason);
        chat_files__mark_local_chat_file_card_failed(e.data.local_message_id, "not sent: " + e.data.reason);
    }
    else if (e.data.type == "data_processing_worker__chat_picture_send_failed")
    {
        chat__picture_preparation_failed(e.data);
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
        return server_msg.process_file_receive_completed_from_server_from_server(e.data.value);
    }
}
