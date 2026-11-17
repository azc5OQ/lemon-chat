// controls microphone capture, the server WebRTC connection and MP3 streaming
// audio.js owns the sound graph. dispatch.js and ui.js call this module

// state private to this file
// set once the udp-capability probe has run (after the first failed datachannel attempt window)
var webrtc_udp_probe_done = false;

// continue capture briefly after key release so the final word reaches the server
var PTT_RELEASE_HANGOVER_MS = 250;

var ptt_pending_stop_timer = null;

/**
 * @brief sets the microphone mode without starting capture
 * UI code applies microphone and Android side effects
 *
 * @param boolean is_active -> the new value
 *
 * @return void
 */
function voice__set_microphone_always_on(is_active)
{
    g_is_microphone_always_on = (is_active == true);

    // keep a saved local continuous mode preference ahead of the Android setting
    let has_local_choice = false;
    has_local_choice = (utils__storage_get("lemon_continuous_mic") != null);

    if (has_local_choice == false)
    {
        g_is_continuous_mic_mode = g_is_microphone_always_on;
    }
}

/**
 * @brief the datachannel loop's sleep, resolvable early through g_datachannel_retry_sleep_resolve
 *
 * @param number ms -> the sleep in milliseconds
 *
 * @return promise resolves after the sleep or on the early wake
 */
function voice__datachannel_retry_sleep(ms)
{
    return new Promise(function(resolve)
    {
        let timer = setTimeout(function()
        {
            g_datachannel_retry_sleep_resolve = null;
            resolve();
        }, ms);

        g_datachannel_retry_sleep_resolve = function()
        {
            clearTimeout(timer);
            g_datachannel_retry_sleep_resolve = null;
            resolve();
        };
    });
}

/**
 * @brief starts microphone capture and reports transmission when permitted
 * a new press during the release delay cancels the pending stop
 *
 * @return void
 */
function voice__process_start_sending_audio()
{
    if (!groups__can_send_to_channel("talk", g_current_channel_id))
    {
        groups__notify_channel_power_denied("talk");
        return;
    }

    // cancel the pending stop when a new press continues the same transmission
    if (ptt_pending_stop_timer != null)
    {
        clearTimeout(ptt_pending_stop_timer);
        ptt_pending_stop_timer = null;
    }

    if (g_is_microphone_enabled == false)
    {
        return;
    }
    if (g_is_client_microphone_allowed_by_server == false)
    {
        return;
    }

    if (g_last_sent_value_microphone_usage == true)
    {
        return;
    }
    if (g_local_audio_stream == null)
    {
        return;
    }

    if (g_local_audio_stream.getTracks() == null)
    {
        return;
    }

    // jump the sequence with the encoder reset so receivers reset their decoders
    g_voice_send_sequence_number = (g_voice_send_sequence_number + G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP) & 0xffff;

    audio__set_microphone_capture_active(true);
    voice__set_mic_transmitting_visual(true);

    g_local_audio_stream.getTracks()[0].enabled = true;

    try {
        g_audio_recorder_gain_node.connect(g_microphone_recorder);
    } catch (e) {
        console.log("audio_recorder_gain_node.connect" + e.message);
    }
    // g_microphone_recorder.connect(audio_context.destination);

    g_is_microphone_active = true;
    let message_object = {
        message: {
            type: "microphone_usage",
            value: 1,
        }
    };

    connection__send_message_object(message_object);
    g_last_sent_value_microphone_usage = 1;
}

/**
 * @brief shows transmission on talk controls without changing the enable toggle
 *
 * @param boolean is_transmitting -> true while audio is going out
 *
 * @return void
 */
function voice__set_mic_transmitting_visual(is_transmitting)
{
    let button_ids = ["microphone-push-to-talk-button-touch-device", "microphone-always-broadcasting-audio-button"];

    for (let i = 0; i < button_ids.length; i++)
    {
        let button = document.getElementById(button_ids[i]);

        if (button != null)
        {
            if (is_transmitting == true)
            {
                button.classList.add("mic-transmitting");
            }
            else
            {
                button.classList.remove("mic-transmitting");
            }
        }
    }
}

/**
 * @brief schedules capture to stop after the final audio frames have been sent
 *
 * @return void
 */
function voice__process_stop_sending_audio()
{
    if (ptt_pending_stop_timer != null)
    {
        clearTimeout(ptt_pending_stop_timer);
    }

    ptt_pending_stop_timer = setTimeout(function()
    {
        ptt_pending_stop_timer = null;
        voice__process_stop_sending_audio_now();
    }, PTT_RELEASE_HANGOVER_MS);
}

/**
 * @brief the actual stop: disables the mic track (the capture graph stays connected) and reports microphone_usage=2 to the server
 *
 * @return void
 */
function voice__process_stop_sending_audio_now()
{
    // capture exists only in the WebView
    if (typeof audio__set_microphone_capture_active !== "function")
    {
        return;
    }

    audio__set_microphone_capture_active(false);
    voice__set_mic_transmitting_visual(false);

    if (g_is_microphone_enabled == false)
    {
        return;
    }
    if (g_is_client_microphone_allowed_by_server == false)
    {
        return;
    }

    if (g_local_audio_stream == null)
    {
        return;
    }

    if (g_local_audio_stream.getTracks() == null)
    {
        return;
    }

    // activate recorder by assigning function to onaudioprocess

    g_local_audio_stream.getTracks()[0].enabled = false;

    // keep the capture graph connected so Chromium cannot replay buffered speech
    // the worklet gate controls transmission

    g_is_microphone_active = false;
    let message_object = {
        message: {
            type: "microphone_usage",
            value: 2,
        }
    };

    connection__send_message_object(message_object);
    g_last_sent_value_microphone_usage = 2;

    // discard partial encoder input before the next transmission
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
}

/**
 * @brief probes UDP support after a failed data channel attempt and reports missing candidates
 *
 * @return void
 */
function voice__probe_webrtc_udp_and_warn()
{
    if (webrtc_udp_probe_done == true)
    {
        return;
    }
    webrtc_udp_probe_done = true;

    try
    {
        let probe_pc = new RTCPeerConnection();
        let got_udp_candidate = false;

        probe_pc.onicecandidate = function(event)
        {
            if (event.candidate != null && event.candidate.candidate.toLowerCase().indexOf(" udp ") != -1)
            {
                got_udp_candidate = true;
            }
        };

        probe_pc.createDataChannel("udp-probe");
        probe_pc.createOffer().then(function(offer)
        {
            return probe_pc.setLocalDescription(offer);
        });

        window.setTimeout(function()
        {
            probe_pc.close();

            if (got_udp_candidate == false)
            {
                let reason = "audio data blocked: the browser refuses direct UDP - possibly a chrome://flags setting like 'disable non-proxied UDP'. this is just a UDP websocket to your server, never a connection to other users";
                console.warn(reason);
                utils__custom_log(reason);
                utils__custom_alert(reason);
            }
        }, 3000);
    }
    catch (probe_error)
    {
        console.warn("webrtc udp probe could not run: " + probe_error.message);
    }
}

/**
 * @brief retries the server peer connection while voice is allowed and the client is active
 *
 * @param boolean is_this_reconnect -> true when a previous channel existed
 *
 * @return promise resolves when the loop ends
 */
async function voice__webrtc_datachannel_connection_check(is_this_reconnect = false)
{
    audio_transport__start();
    // the datachannel is needed for voice over webrtc, as the fallback of a websocket that does not come up, or for video
    if (audio_transport__wants_webrtc() === false)
    {
        g_is_webrtc_datachannel_check_running = false;
        return;
    }

    // Node has no WebRTC capture. release the running flag before returning
    if (typeof RTCPeerConnection === "undefined")
    {
        g_is_webrtc_datachannel_check_running = false;
        return;
    }

    // use an unordered data channel without retransmission, connected only to the server

    while (true)
    {
        console.log("voice__webrtc_datachannel_connection_check ran");
        if (audio_transport__wants_webrtc() === false)
        {
            g_is_webrtc_datachannel_check_running = false;
            break;
        }

        if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
        {
            // keep retrying after setup exceptions instead of leaving the running flag stuck
            try
            {
                voice__create_new_peer_connection_object_for_use(is_this_reconnect);

                let message_object = {
                    message: {
                        type: "create_new_webrtc_datachannel_connection",
                    }
                };

                connection__send_message_object(message_object);
            }
            catch (datachannel_attempt_error)
            {
                console.error("webrtc datachannel attempt failed: " + (datachannel_attempt_error != null && datachannel_attempt_error.stack ? datachannel_attempt_error.stack : datachannel_attempt_error));
            }

            await voice__datachannel_retry_sleep(10000);

            // after the first failed attempt, check whether UDP candidates are available
            if (g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
            {
                // with a websocket as the fallback the warning about blocked udp is not needed
                if (g_audio_transport.mode === "webrtc" && g_server_policy.is_audio_over_websocket_allowed !== true)
                {
                    voice__probe_webrtc_udp_and_warn();
                }
            }

            // after repeated refusals, wait for a new login before trying again
            if (g_is_webrtc_datachannel_connected == false && g_webrtc_datachannel_cooldown_until_ms > new Date().valueOf())
            {
                let remaining_ms = g_webrtc_datachannel_cooldown_until_ms - new Date().valueOf();
                console.log("datachannel: server cooldown, next attempt in " + Math.ceil(remaining_ms / 1000) + " s");
                await voice__datachannel_retry_sleep(remaining_ms);
            }
        }
        else
        {
            console.log("voice__webrtc_datachannel_connection_check break");
            g_is_webrtc_datachannel_check_running = false;
            break;
        }
    }
}

/**
 * @brief ships the local webrtc sdp answer to the server
 *
 * @param string value -> the sdp answer
 *
 * @return void
 */
function voice__send_sdp_answer_to_server(value)
{
    let message_object = {
        message: {
            type: "sdp_answer",
            value: value
        }
    };


    connection__send_message_object(message_object);
}

/**
 * @brief replaces the old peer connection and installs handlers on its successor
 *
 * @param boolean is_this_reconnect -> true when a previous channel existed
 *
 * @return void
 */
function voice__create_new_peer_connection_object_for_use(is_this_reconnect = false)
{
    // close the replaced peer connection so its callbacks cannot overwrite the new state
    if (g_peer_connection_with_server != null)
    {
        try
        {
            g_peer_connection_with_server.removeEventListener('datachannel', webrtc.peerconnection_on_datachannel_receive);
            g_peer_connection_with_server.close();
        }
        catch (peer_connection_close_error)
        {
            console.log("closing replaced peer connection failed: " + peer_connection_close_error.message);
        }
    }

    g_datachannel = null;
    g_video_datachannel = null;
    g_is_video_datachannel_connected = false;
    g_peer_connection_with_server = null;

    // add the constraint only when absent because audio_enabled may rebuild the ICE configuration
    let is_turn_server_present = false;

    for (let i = 0; i < g_iceconfig.iceServers.length; i++)
    {
        let ice_server_urls = g_iceconfig.iceServers[i].urls;

        if ((typeof ice_server_urls === "string" && ice_server_urls.indexOf("turn:") == 0) || (Array.isArray(ice_server_urls) && ice_server_urls.length > 0 && String(ice_server_urls[0]).indexOf("turn:") == 0))
        {
            is_turn_server_present = true;
            break;
        }
    }

    if (is_turn_server_present == false && g_iceconfig.iceServers.length > 0)
    {
        let turn_server = {
            urls: [
                "turn:"+g_host+":3478?transport=udp",
                "turn:"+g_host+":3478?transport=tcp"
            ],
            username: "usweger123",
            credential: "pw1wegweg23Q"
        };

        g_iceconfig.iceServers.push(turn_server);
    }

    g_peer_connection_with_server = new RTCPeerConnection(g_iceconfig);
    g_peer_connection_with_server.onicecandidate = webrtc.peerconnection_handle_ice_candidate_event;
    g_peer_connection_with_server.onsignalingstatechange = webrtc.peerconnection_handle_signaling_state_change_event;
    g_peer_connection_with_server.onicegatheringstatechange = webrtc.peerconnection_handle_ice_gathering_state_change_event;
    g_peer_connection_with_server.oniceconnectionstatechange = webrtc.peerconnection_handle_ice_connection_state_change_event;
    g_peer_connection_with_server.onnegotiationneeded = webrtc.peerconnection_handle_negotiation_needed_event;
    g_peer_connection_with_server.onconnectionstatechange = webrtc.peer_connection_handle_onconnection_state_change_event;
    g_peer_connection_with_server.addEventListener('datachannel', webrtc.peerconnection_on_datachannel_receive);
}

/**
 * @brief shows the floating microphone control when connected and allowed by local settings
 * the visibility preference applies to the Mobile theme only
 *
 * @return void
 */
function voice__update_microphone_button()
{
    let hide_in_mobile_theme = connection__current_theme_name() === "default-mobile" && g_hide_mobile_microphone_button;
    document.body.classList.toggle("hide-mobile-microphone-button", hide_in_mobile_theme);

    let button = document.getElementById("microphone-push-to-talk-button-touch-device");

    if (button == null)
    {
        return;
    }

    if (g_is_client_running_under_touch_device == false || g_hide_microphone_button == true || hide_in_mobile_theme)
    {
        button.style.display = "none";
        return;
    }

    // hide the microphone while disconnected
    if (g_is_authenticated != true)
    {
        button.style.display = "none";
        return;
    }

    button.style.display = "block";
    button.classList.toggle("mic-unavailable", g_is_microphone_available == false);
}

/**
 * @brief requests microphone access, builds capture and reports readiness to the server
 *
 * @return void
 */
function voice__activate_microphone()
{
    // report unavailable microphone APIs before calling getUserMedia
    if (navigator.mediaDevices == null || typeof navigator.mediaDevices.getUserMedia != "function")
    {
        utils__custom_alert("the microphone needs a secure connection: open the client over HTTPS, from localhost, or as a local file. you are most likely loading it over plain HTTP.");
        return;
    }

    let microphone_constraints = { audio: true };

    // request the device as ideal so an unplugged device can fall back to the default
    if (g_selected_microphone_device_id != "")
    {
        microphone_constraints = { audio: { deviceId: { ideal: g_selected_microphone_device_id } } };
    }

    navigator.mediaDevices.getUserMedia(microphone_constraints)
    .then(
        function (stream)
        {
            if (g_alert_push_to_talk_key_shown_once == false)
            {
                // only display the alert if touch device is not used
                if (!g_is_client_running_under_touch_device)
                {
                    utils__custom_alert("press Q to talk");
                }
                g_alert_push_to_talk_key_shown_once = true;
            }

            document.getElementById("play-pause-song-container").style.visibility = "visible";
            document.getElementById("stop-song").style.display = "none";
            document.getElementById("custom-file-upload-button-song").style.visibility = "visible";
            document.getElementById("custom-file-upload-button-song").style.display = "block";

            // only do this once
            if (g_microphone_recorder == null)
            {
                g_opus_encoder_worker.postMessage({
                    type: "init",
                    sampleRate: g_audio_context.sampleRate
                });

                stream.getTracks()[0].enabled = false;

                g_local_audio_stream = stream;
                g_audio_input = g_audio_context.createMediaStreamSource(stream);
                g_audio_recorder_gain_node = g_audio_context.createGain();

                // create worklet capture when available, otherwise use ScriptProcessorNode
                // connect microphone, gain, capture and destination
                audio__create_microphone_capture_node();

                const audioTracks = stream.getAudioTracks();

                utils__custom_log('Using audio device: ' + audioTracks[0].label);
                stream.oninactive = function ()
                {
                    utils__custom_log('Stream ended');
                };
            }

            if (!g_is_microphone_enabled)
            {
                sounds__play("microphone_activated");
            }
            g_is_microphone_enabled = true;

            document.getElementById("play-pause-song-container").style.visibility = "visible";
            document.getElementById("toggle-microphone-label").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAE30lEQVR4Xu2ae+jfUxjHX2MyImxJ5hKSrLX5T/xpNZeUkcsfiCSstTFrbRTij03mtpFEolzKJffLpqTUUlptjBqR27BFsdxyp3c7nzy/0/f3+5znfM75RPuc/z7f33Oe5znv53p+55lEv+twYB5wOnAEcGgQ/wXwKfAS8DywtS+1JvUk6BDgBuBSYPcWmX8BTwPLAihVVewDgDOBR4B9nCf5AbgQeMG5z0VeG4CrgDuA3Vxa/Ussb7gauCtzf+u2mgDI8nLl3MM3yguEs2p5Qi0AlNy2ZLj9eBZTOBwLfNVqUidBLQAeBC5x6tJG/gBwWRuR9+81AFCp+zgh23t1/TOUTpXMYqsGAFcCa4ppOJbRQuCekrxrALAWOLWkkobXK6GJKsa+BgAfAkcX03Asow9CMizGvgYAytjepif1QD8C+6YSp9DVAODvFMEdaIrqXJRZONQAQAfrpmwtarSizAYP2InAEAIpftyBpqjXFmU2hMAQAkMOGJLgUAV2gTIY1/m4kvTdB7TpM2HFzSmDbQIHADo0OSlb2zzOZVQX8TiNTptCKYfy0LTJc53JRTwAMLrTa7OIx7optG3yXEZ1EQ8e8N/zgMnA75HbuIzqIg6CfgP2MEKnAL+a75r/FP0e2M/IOjh6LpNue6bEUUOTA8B3wP5GyEHA1+Zb/7o+xqOEg/Z9YIahnw28Y76/BaY5+JEDwHvATCPkBOAt893nw4heoJ81st8FBEryygFAAiW4WVcA95tvPV/dnayBj3ABcK/ZshrQDEKzngHO9rDMAeBaYKUR8hRwnvk+DPikwuPoH8CRgH0cfRs4zsheDqyqDcCJwJtGiBLTdOAn85uesjUPVHLdB8w3DBWGcnlrxOOBDR6hOR6giQ9NcenQ44WBhqI0IFHqGUsga0Bim5EpQC433/IMPc27LmM5AEimJr5uMsI1D6DsrDLUrNOAFwuEgkZklHPEq1kqfx8Be5vfrgNWeKwv2lwApgKfRY+g1wC3RApoVuDODnNCOvziEUn1SeBcI0u9h6y/oy8AJOd2YIkRqGZIMbg5UuIM4NGMcJDbXxCGJy1LDVo+F8m4NcwVes+f7QESpGZIh1XWb5amPZUkt0eaHAjIRVXG1L5OtGT1xwBldBvz2jMLeAM4wDD4MvyuBs29ckOgETQXeDUKJYFyyggQtEfTY82orEqaHZVVHnk5jMONmgM6ClgPKP6bpYR3MvCa++RhQ1cAxEYxrji1S4c5B9iUq1i0bw7wOCBPskuybRi6xZUAQLO/DwPnR9J/AW4MANnq4FFyL2BpqDpx6ChMLgY0PZa9SgAg4VJOCtmOsFFK5eq2MC/8c6KmKm8XAddH/Uaz/YmQIDsdXsxKAdCAoDosi40aj9V8jy5KrwMbQzP1Tbi+aqZIOUEXGY3SK7fI+vFSglSrK2DUGndeJQFolFG8ajrcdoqdFQ1Xbrn8uhLMGh41ABBv3cmVnBZl1P/4fOoHdLvU1Lnu+0VXLQAaJdUx6rqquWHbL6Qc4nPgoTAqX/zgtT0gPqCAVpd4EnBzy+l13Vae0K3OdbFJQXWUYjn7uuxpO1Rtrxyje6/CguQBgBb36dUovQobPGAnAkMIDCEwMQK9hmWvwnaVHNAW4116CO0tarSizBItPADQEYGiRivK7P/oAf8Ae0zZQdfLrKYAAAAASUVORK5CYII=)";

            let message_object = {
                message: {
                    type: "microphone_usage",
                    value: 2,
                }
            };

            let message_json_string = connection__process_message_before_sending(message_object);
            let data = keys__encrypt_all_message_data_and_convert_to_base64(message_json_string);

            connection__websocket_worker_send(data);

            g_last_sent_value_microphone_usage = 2;

            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_enabled_on_touch_device = true;
            }

            // continuous mode still requires a tap before transmission starts

            // apply a pending continuous transmission request after microphone permission succeeds
            if (g_is_continuous_transmission_active == true)
            {
                voice__process_start_sending_audio();
            }

        },
        function (fail) {
            console.log("voice__activate_microphone is acting weird");
            console.log(fail);
        }
    )
    .catch(function (error)
    {
        const errorMessage = 'navigator.MediaDevices.getUserMedia error: ' + error.message + ' ' + error.name;
        console.log(errorMessage);
        sounds__play("error");
        document.getElementById("toggle-microphone").checked = false;
    });
}

/**
 * @brief updates a client audio indicator, local talk controls and any song marquee
 *
 * @param object client -> the client_id and audio_state from the server
 *
 * @return void
 */
function voice__process_audio_state_of_single_client(client)
{

    let target_element = document.getElementById("client-audio-state-" + client.client_id);

    // ignore audio state for a client that is not listed yet
    let client_index = channel_tree__get_client_index_in_array_by_client_id(client.client_id);

    if (client_index == -1 || target_element == null)
    {
        return;
    }

    g_client_list[client_index].audio_state = client.audio_state;

    if (client.audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
    {

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-active");

    }
    else if (client.audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ENABLED)
    {

        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-enabled");

        if (client.client_id == g_local_client_id)
        {
            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_available = true;
                voice__update_microphone_button();
            }
            else
            {
                document.getElementById("toggle-microphone-label").style.display = "block";
            }
        }
    }
    else if (client.audio_state == G_AUDIO_STATE.PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS)
    {
        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-microphone-disabled");

        if (client.client_id == g_local_client_id)
        {
            if (g_is_client_running_under_touch_device)
            {
                g_is_microphone_available = true;
                voice__update_microphone_button();
            }
            else
            {
                document.getElementById("toggle-microphone-label").style.display = "block";
            }
        }

    }

    else if (client.audio_state == G_AUDIO_STATE.AUDIO_COMPLETELY_DISABLED)
    {
        if (target_element.classList.contains("client-audio-state-microphone-active"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-active");
        }

        if (target_element.classList.contains("client-audio-state-microphone-enabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-enabled");
        }

        if (target_element.classList.contains("client-audio-state-microphone-disabled"))
        {
            document.getElementById("client-audio-state-" + client.client_id).classList.remove("client-audio-state-microphone-disabled");
        }

        document.getElementById("client-audio-state-" + client.client_id).classList.add("client-audio-state-completely-disabled");

        if (client.client_id == g_local_client_id && audio_transport__active() == null)
        {
            g_is_microphone_available = false;
            voice__update_microphone_button();
            document.getElementById("toggle-microphone-label").style.display = "none";

            // audio is enabled by the server and server just disconnected our audio, attempt datachannel reconnect
            if (g_is_voice_chat_allowed_by_server == true)
            {
                g_is_webrtc_datachannel_connected = false;
                if (g_is_webrtc_datachannel_check_running == false)
                {
                    // claim the running flag before starting the retry loop
                    g_is_webrtc_datachannel_check_running = true;
                    voice__webrtc_datachannel_connection_check(true);
                }
            }
        }
    }

    if (client.is_streaming_song)
    {
        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + client.client_id + '"]');
        if (element != null)
        {
            element.style.display = "inline-block";
            document.getElementById("marquee-song-name-client-id-" + client.client_id).innerHTML = chat__sanitize_string(client.song_name);
        }
        else
        {
            console.log("could not find element");
        }
    }
}

// samples sent to the encoder per step of the song stream
const VOICE_SONG_CHUNK_SAMPLES = 4096;

/**
 * @brief paces the samples of a song through the encoder until the song ends or the server stops it. Each chunk
 * is converted to floating point just before it is sent, so the song is held in memory once, as 16 bit samples
 *
 * @param Int16Array pcm -> the song, one channel
 * @param number mp3_sample_rate -> the song's sample rate
 *
 * @return promise resolves when the song ended or was stopped
 */
async function voice__stream_local_mp3_file_to_other_clients(pcm, mp3_sample_rate)
{
    // reset the encoder and sequence at song start so receivers discard old decoder state
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
    g_voice_send_sequence_number = (g_voice_send_sequence_number + G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP) & 0xffff;

    for (let start = 0; start < pcm.length; start += VOICE_SONG_CHUNK_SAMPLES)
    {
        // convert one chunk
        let chunk = new Float32Array(VOICE_SONG_CHUNK_SAMPLES);
        let end = Math.min(pcm.length, start + VOICE_SONG_CHUNK_SAMPLES);
        for (let i = start; i < end; i++)
        {
            chunk[i - start] = (pcm[i] + 0.5) / 32767.5;
        }

        // send to encoder
        g_opus_encoder_worker.postMessage({
            type: "encode_mp3_chunk",
            value: chunk,
            mp3_sample_rate: mp3_sample_rate
        }, [chunk.buffer]);

        if (g_stop_song_stream_message_received)
        {
            g_stop_song_stream_message_received = false;

            // pause or microphone shutdown arrives as stop_song_stream while this loop waits
            // handle that stop separately from reaching the end of the song

            // keep a disabled microphone disabled when pausing the song

            if (g_client_list[channel_tree__get_client_index_in_array_by_client_id(g_local_client_id)].audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
            {
                g_is_microphone_active = true;
                is_playing_music = false;
                g_last_sent_value_microphone_usage = 2;

                let message_object2 = {
                    message: {
                        type: "microphone_usage",
                        value: 2,
                    }
                };

                connection__send_message_object(message_object2);
            }

            return;
        }

        // sending finishes before remote playback, so the microphone may stop while listeners hear the buffered tail
        await utils__sleep(80);
    }

    // this is case 3 when song ends because it finished playing
    // update marquee animation and microphone state for other users

    let message_object1 = {
        message:
        {
            type: "stop_song_stream"
        }
    };

    connection__send_message_object(message_object1);

    document.getElementById("custom-file-upload-button-song").style.display = "inline-block";
    document.getElementById("stop-song").style.display = "none";

    g_is_microphone_active = true;
    is_playing_music = false;
    g_last_sent_value_microphone_usage = 2;

    let message_object3 = {
        message: {
            type: "microphone_usage",
            value: 2,
        }
    };

    connection__send_message_object(message_object3);
}

/**
 * @brief tells whether file data starts like an MP3 file: with an ID3 tag or with the header of an MPEG audio
 * frame (11 set bits, the frame sync). The file name is not used: a file named .m4a can hold MP3 data and the reverse
 *
 * @param Uint8Array bytes -> the first bytes of the file, at least 3
 *
 * @return boolean true for MP3 data
 */
function voice__is_mp3_data(bytes)
{
    // id3 tag
    if (bytes.length >= 3 && bytes[0] == 0x49 && bytes[1] == 0x44 && bytes[2] == 0x33)
    {
        return true;
    }

    // frame sync
    return bytes.length >= 2 && bytes[0] == 0xff && (bytes[1] & 0xe0) == 0xe0;
}
