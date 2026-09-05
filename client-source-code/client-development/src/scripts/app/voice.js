// voice.js is embedded in template.html along with the other client files
// it is the voice side of the page: the microphone (push-to-talk, always-on), the webrtc datachannel
// to the server that carries audio both ways, and the music bot mp3 stream
// audio.js owns the sound graph and the workers; dispatch.js and ui.js call in here

// state private to this file
// set once the udp-capability probe has run (after the first failed datachannel attempt window)
var webrtc_udp_probe_done = false;

// push-to-talk release hangover: capture keeps running this long after the key is let go, so word
// tails ("hello" not "hell") ship before the mic-off message stops the relay
var PTT_RELEASE_HANGOVER_MS = 250;

var ptt_pending_stop_timer = null;

/**
 * @brief the one writer of g_is_microphone_always_on
 *        the side effects (mic, android bridge, repaint) stay in ui.js, so a headless runtime can
 *        own the flag without touching the microphone
 *
 * @param boolean is_active -> the new value
 *
 * @return void
 */
function voice__set_microphone_always_on(is_active)
{
    g_is_microphone_always_on = (is_active == true);

    // the android settings radio maps onto the new tap-to-toggle mode; a choice made in
    // the local settings panel (localStorage) outranks it
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
 * @brief push-to-talk press: enables the mic track, connects the recorder chain and reports microphone_usage=1 to the server
 *        no-op when the mic is off, forbidden or already sending; a re-press inside the release
 *        hangover cancels the pending stop
 *
 * @return void
 */
function voice__process_start_sending_audio()
{
    // a re-press inside the release hangover cancels the pending stop: the mic never
    // went down, so the early-outs below just keep the running capture untouched
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

    // spurt-start marker: receivers scrub this sender's decoder on the jump, pairing with
    // the encoder reset that the capture-start clear below performs
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
 * @brief the glow on the mic controls while actually transmitting, so the user knows the mic is hot
 *
 * @param boolean is_transmitting -> true while audio is going out
 *
 * @return void
 */
function voice__set_mic_transmitting_visual(is_transmitting)
{
    let button_ids = ["microphone-push-to-talk-button-touch-device", "microphone-always-broadcasting-audio-button", "toggle-microphone-label"];

    for (let i = 0; i < button_ids.length; i++)
    {
        let button = document.getElementById(button_ids[i]);

        if (button != null)
        {
            if (is_transmitting == true) { button.classList.add("mic-transmitting"); }
            else { button.classList.remove("mic-transmitting"); }
        }
    }
}

/**
 * @brief push-to-talk release: keeps capturing for a short hangover so the tail of the last word ships (and the mic-off message cannot outrun the final audio frames), then stops
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
 * @brief the actual stop: disables the mic track (the capture graph stays wired) and reports microphone_usage=2 to the server
 *
 * @return void
 */
function voice__process_stop_sending_audio_now()
{
    // audio capture only exists in the webview; node crashed here on a missing symbol
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

    // no edge of the capture graph may ever be disconnected: an unpulled mic source freezes stale
    // speech in chromium's stream fifo, and the next press replays it. the worklet gate mutes alone

    g_is_microphone_active = false;
    let message_object = {
        message: {
            type: "microphone_usage",
            value: 2,
        }
    };

    connection__send_message_object(message_object);
    g_last_sent_value_microphone_usage = 2;

    // drop the encoder's half-filled frame, otherwise its stale samples lead the NEXT
    // transmission (a piece of the previous sentence replayed on the new press)
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
}

/**
 * @brief the udp probe, run once after a datachannel attempt failed
 *        a browser under the "disable non-proxied udp" policy yields no udp ice candidates at all,
 *        which the page cannot read but can infer, and then says so
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
        probe_pc.createOffer().then(function(offer) { return probe_pc.setLocalDescription(offer); });

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
 * @brief the datachannel loop: whenever voice is allowed, the channel is down and the client is not idle, it creates a fresh peer connection and asks the server for a new datachannel, then sleeps and looks again
 *        an unordered, unreliable channel between this client and the server only, never between
 *        clients; node has no webrtc and leaves at once
 *
 * @param boolean is_this_reconnect -> true when a previous channel existed
 *
 * @return promise resolves when the loop ends
 */
async function voice__webrtc_datachannel_connection_check(is_this_reconnect = false)
{
    // voice lives in the webview only; node has no webrtc and must never enter this.
    // the caller set the running flag, so it must be dropped here too
    if (typeof RTCPeerConnection === "undefined")
    {
        g_is_webrtc_datachannel_check_running = false;
        return;
    }

    // an unordered, unreliable datachannel (maxRetransmits 0): a udp-like pipe between this client
    // and the server only, never between clients, so nothing here is peer to peer

    while (true)
    {
        console.log("voice__webrtc_datachannel_connection_check ran");

        if (g_is_voice_chat_allowed_by_server == true && g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
        {
            // one throw here must not kill the loop: a dead loop leaves the running flag
            // stranded true, which blocks every future re-create until the app restarts
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
                console.error("webrtc datachannel attempt failed: "
                    + (datachannel_attempt_error != null && datachannel_attempt_error.stack ? datachannel_attempt_error.stack : datachannel_attempt_error));
            }

            await voice__datachannel_retry_sleep(10000);

            // a full attempt window passed and the channel is still down: find out
            // whether the browser is even capable of direct udp before retrying forever
            if (g_is_webrtc_datachannel_connected == false && g_is_deep_idle == false)
            {
                voice__probe_webrtc_udp_and_warn();
            }

            // the server refused for a while (10 attempts never connected): wait it out here instead
            // of asking every 10 s; a new login resolves the sleep and the server counts afresh
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

    console.log(message_object);

    connection__send_message_object(message_object);
}

/**
 * @brief replaces g_peer_connection_with_server with a fresh RTCPeerConnection wired to the webrtc handlers
 *        the replaced one is closed, since an abandoned-but-open pc kept its ice agent alive and
 *        hijacked state with zombie events; g_iceconfig carries the TURN server exactly once
 *
 * @param boolean is_this_reconnect -> true when a previous channel existed
 *
 * @return void
 */
function voice__create_new_peer_connection_object_for_use(is_this_reconnect = false)
{
    // close the replaced pc: an abandoned-but-open pc kept its ice agent and its
    // 'datachannel' listener alive, hijacking state with zombie events and slowly
    // eating into the browser's peer connection cap
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
    g_peer_connection_with_server = null;

    // added when missing rather than keyed on is_this_reconnect: the audio_enabled handler
    // rebuilds g_iceconfig without it, and retries used to push a duplicate every 10s
    let is_turn_server_present = false;

    for (let i = 0; i < g_iceconfig.iceServers.length; i++)
    {
        let ice_server_urls = g_iceconfig.iceServers[i].urls;

        if ((typeof ice_server_urls === "string" && ice_server_urls.indexOf("turn:") == 0)
            || (Array.isArray(ice_server_urls) && ice_server_urls.length > 0 && String(ice_server_urls[0]).indexOf("turn:") == 0))
        {
            is_turn_server_present = true;
            break;
        }
    }

    if (is_turn_server_present == false)
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
 * @brief shows or hides the floating push-to-talk button: touch devices only, never over the connect page, and not when hidden in the local settings
 *
 * @return void
 */
function voice__update_microphone_button()
{
    let button = document.getElementById("microphone-push-to-talk-button-touch-device");

    if (button == null)
    {
        return;
    }

    if (g_is_client_running_under_touch_device == false || g_hide_microphone_button == true)
    {
        button.style.display = "none";
        return;
    }

    // the connect screen has nobody to talk to, so the mic has no business floating over it
    let connect_page = document.getElementById("verification-system");

    if (connect_page != null && connect_page.style.display != "none")
    {
        button.style.display = "none";
        return;
    }

    button.style.display = "block";
    button.classList.toggle("mic-unavailable", g_is_microphone_available == false);
}

/**
 * @brief requests getUserMedia (with a plain-http explainer when unavailable), builds the mic capture chain once, and reports microphone_usage=2 (enabled, not sending) to the server
 *        a chosen input device rides along as "ideal", so an unplugged device falls back to the default instead of failing
 *
 * @return void
 */
function voice__activate_microphone()
{
    // getUserMedia exists only in a secure context (https, localhost or a file:// page); over plain
    // http from a remote host navigator.mediaDevices is undefined, so say so instead of throwing
    if (navigator.mediaDevices == null || typeof navigator.mediaDevices.getUserMedia != "function")
    {
        utils__custom_alert("the microphone needs a secure connection: open the client over HTTPS, from localhost, or as a local file. you are most likely loading it over plain HTTP.");
        return;
    }

    let microphone_constraints = { audio: true };

    // a chosen input device rides along as "ideal", because "exact" would fail the whole mic
    // activation when that device is unplugged - the browser then falls back to its default
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

                // AudioWorkletNode capture when the worklet module is available, ScriptProcessorNode
                // otherwise; also wires mic -> gain -> capture node -> destination
                audio__create_microphone_capture_node();

                const audioTracks = stream.getAudioTracks();

                utils__custom_log('Using audio device: ' + audioTracks[0].label);
                stream.oninactive = function ()
                {
                    utils__custom_log('Stream ended');
                };
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

            // legacy always-on used to hide the button and open the mic right here;
            // continuous mode is a tap-toggle now, so the button stays and stays quiet

            // desktop continuous armed the flag before getUserMedia resolved, so the
            // start call found g_is_microphone_enabled false and returned silently
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
        document.getElementById("toggle-microphone").checked = false;
    });
}

/**
 * @brief applies a client's new audio state: swaps the mic-state css class on their tree row, toggles the local push-to-talk controls, and shows the song marquee when streaming
 *
 * @param object client -> the client_id and audio_state from the server
 *
 * @return void
 */
function voice__process_audio_state_of_single_client(client)
{

    let target_element = document.getElementById("client-audio-state-" + client.client_id);

    // an audio state can arrive for a client this runtime has not listed yet; the lookup
    // returns -1 and g_client_list[-1] is undefined, which threw out of the handler
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

        if (client.client_id == g_local_client_id)
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
                    // claim the flag like every other starter - without this the loop ran
                    // unregistered and later triggers stacked concurrent loops on top of it
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

/**
 * @brief paces the song's mp3 chunks into the opus encoder worker
 *        bails out early when the server sends stop_song_stream, otherwise announces the song end after the last chunk
 *
 * @param array data_chunks -> the mp3 chunks
 * @param number mp3_sample_rate -> the song's sample rate
 *
 * @return promise resolves when the song ended or was stopped
 */
async function voice__stream_local_mp3_file_to_other_clients(data_chunks, mp3_sample_rate)
{
    // a song stream is a fresh spurt like a press: reset the encoder and mark the
    // boundary, so receivers scrub this sender's decoder before the first music frame
    g_opus_encoder_worker.postMessage({ type: "clear_opus_encoder_buffer" });
    g_voice_send_sequence_number = (g_voice_send_sequence_number + G_OPUS_SPURT_BOUNDARY_SEQUENCE_JUMP) & 0xffff;

    for (var i = 0; i < data_chunks.length; i++)
    {
        let message = {
            type: "encode_mp3_chunk",
            value: data_chunks[i],
            mp3_sample_rate: mp3_sample_rate
        };

        g_opus_encoder_worker.postMessage(message);

        if (g_stop_song_stream_message_received)
        {
            g_stop_song_stream_message_received = false;

            // a song stream ends three ways: the pause button, the microphone being switched off, or
            // all bytes sent; each is handled a little differently

            // the first two cases arrive as the server's stop_song_stream message, which sets
            // g_stop_song_stream_message_received while this loop awaits its sleep

            // if condition is met only if user clicked "pause song" button located near file image select button
            // if user clicks de-activate microphone button, audio_state is set to 3 (deactivated), not checking audio_state
            // would cause the state go from 3 to 2 again..

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

        // the loop finishes sending before the others finish playing, so the mic-off request goes
        // out while the song is still audible on their side

        // also works with 60
        // 100 - starts lagging
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
 * @brief wav ArrayBuffer -> chunks of float samples: skips the 44-byte header and converts the 16-bit signed samples to floats
 *
 * @param ArrayBuffer arrayBuffer -> the wav file
 * @param number chunkLength -> samples per chunk
 *
 * @return array the Float32Array chunks
 */
function voice__chunk_buffers(arrayBuffer, chunkLength)
{
    var chunkedBuffers = [];

    var totalFile = new Int16Array(arrayBuffer);
    // Skip wave header; 44 bytes
    for (i = 22; i < totalFile.length; i += chunkLength)
    {

        // Convert 16 bit signed int to 32bit float
        var bufferChunk = new Float32Array(chunkLength);
        for (j = 0; j < chunkLength; j++)
        {
            bufferChunk[j] = (totalFile[i + j] + 0.5) / 32767.5;
        }

        chunkedBuffers.push(bufferChunk);
    };

    return chunkedBuffers;
}
