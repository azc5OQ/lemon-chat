// carries voice through WebRTC or the /audio WebSocket, within server policy
// try the allowed fallback when the preferred transport stays down. return to the preferred transport when it recovers
// the chat connection requests WebSocket sessions. failed attachments retry with increasing delays
// echo empty WebSocket keepalives and probe quiet data channels to detect stalled connections
// report the local microphone state after attachment because the server resets it to listening
// session encryption authenticates each frame. channel encryption keeps voice contents private from the server
// keys.js and server crypto_media.c define the encryption and frame layout
var AUDIO_TRANSPORT_FALLBACK_AFTER_MS = 10000;
var AUDIO_TRANSPORT_REQUEST_INTERVAL_MS = 10000;
var AUDIO_TRANSPORT_REQUEST_INTERVAL_MAX_MS = 60000;
var AUDIO_TRANSPORT_DATACHANNEL_PROBE_MS = 5000;
var AUDIO_TRANSPORT_DATACHANNEL_DEAD_MS = 15000;
// maximum frame content. encryption adds 29 bytes within the server's 4132 byte limit
var AUDIO_TRANSPORT_MAX_CONTENT_BYTES = 4080;

/**
 * @brief checks whether the server enables voice or music bot audio
 *
 * @return boolean true when voice chat or music bot audio is enabled
 */
function audio_transport__is_audio_on()
{
    return g_server_policy.is_voice_chat_active === true || g_server_policy.is_music_bot_audio_active === true;
}

/**
 * @brief checks whether the voice data channel is open
 *
 * @return boolean -> true when the data channel is open
 */
function audio_transport__is_datachannel_open()
{
    return g_datachannel != null && g_datachannel.readyState === "open";
}

/**
 * @brief checks whether the preferred transport has been down long enough to try a fallback
 *
 * @return boolean true when the preferred transport has been down long enough
 */
function audio_transport__is_fallback_due()
{
    return Date.now() - g_audio_transport.preferred_down_since >= AUDIO_TRANSPORT_FALLBACK_AFTER_MS;
}

/**
 * @brief chooses the ready preferred audio connection or an allowed fallback
 *
 * @return string "webrtc", "websocket" or null
 */
function audio_transport__active()
{
    let state = g_audio_transport;

    if (state.mode === "none")
    {
        return null;
    }

    if (state.mode === "webrtc")
    {
        if (audio_transport__is_datachannel_open())
        {
            return "webrtc";
        }
        return state.ready ? "websocket" : null;
    }

    if (state.ready)
    {
        return "websocket";
    }

    if (g_server_policy.is_audio_over_webrtc_allowed === true && audio_transport__is_datachannel_open())
    {
        return "webrtc";
    }

    return null;
}

/**
 * @brief checks whether voice or video still requires the WebRTC connection
 *
 * @return boolean true when video, preference or fallback requires WebRTC
 */
function audio_transport__wants_webrtc()
{
    let state = g_audio_transport;

    // video keeps the peer connection only while it does not prefer the video websocket (video-transport.js)
    if (g_server_policy.is_video_streaming_allowed === true && (typeof video_transport__prefers_websocket !== "function" || video_transport__prefers_websocket() === false))
    {
        return true;
    }

    if (state.mode === "webrtc")
    {
        return true;
    }

    return state.mode === "websocket" && g_server_policy.is_audio_over_webrtc_allowed === true && state.ready === false && audio_transport__is_fallback_due();
}

/**
 * @brief checks whether policy and preference require an audio WebSocket
 *
 * @return boolean true when policy and preference or fallback require a WebSocket
 */
function audio_transport__wants_websocket()
{
    let state = g_audio_transport;

    if (g_server_policy.is_audio_over_websocket_allowed !== true)
    {
        return false;
    }

    if (state.mode === "websocket")
    {
        return true;
    }

    return state.mode === "webrtc" && audio_transport__is_datachannel_open() === false && audio_transport__is_fallback_due();
}

/**
 * @brief starts the WebRTC connection loop when needed and not already running
 *
 * @return void
 */
function audio_transport__start_webrtc_if_wanted()
{
    // keep one connection attempt running at a time
    if (g_is_webrtc_datachannel_connected === true || audio_transport__is_datachannel_open())
    {
        return;
    }
    if (audio_transport__wants_webrtc() === false || g_is_webrtc_datachannel_check_running == true || g_is_deep_idle == true || g_is_authenticated == false || g_is_voice_chat_allowed_by_server != true || typeof RTCPeerConnection === "undefined")
    {
        return;
    }

    g_is_webrtc_datachannel_check_running = true;
    voice__webrtc_datachannel_connection_check(true);
}

/**
 * @brief activates the opened data channel and updates fallback and microphone state
 *
 * @return void
 */
function audio_transport__on_datachannel_open()
{
    let state = g_audio_transport;

    state.datachannel_last_received = Date.now();
    state.datachannel_last_probe = 0;

    if (state.mode === "webrtc" && state.ready === true)
    {
        utils__custom_log("Audio back on the datachannel");
        audio_transport__drop_socket();
    }

    if (audio_transport__active() === "webrtc")
    {
        audio_transport__on_ready();
    }
}

/**
 * @brief records data channel activity and plays frames other than probe replies
 * accept server audio from either transport while a switch is in progress
 *
 * @param ArrayBuffer data -> the frame
 *
 * @return void
 */
function audio_transport__on_datachannel_message(data)
{
    g_audio_transport.datachannel_last_received = Date.now();

    if (data instanceof ArrayBuffer && data.byteLength > 1 && g_audio_transport.mode !== "none")
    {
        audio_transport__receive(data);
    }
}

/**
 * @brief probes a quiet data channel and rebuilds it when the server stops responding
 *
 * @param number now -> date.now()
 *
 * @return void
 */
function audio_transport__probe_datachannel(now)
{
    let state = g_audio_transport;

    // a channel seen open before any frame or open callback: the clock starts now
    if (state.datachannel_last_received === 0)
    {
        state.datachannel_last_received = now;
    }
    if (now - state.datachannel_last_received > AUDIO_TRANSPORT_DATACHANNEL_DEAD_MS)
    {
        audio_transport__restart_datachannel("gave no answer for " + Math.round(AUDIO_TRANSPORT_DATACHANNEL_DEAD_MS / 1000) + " s");
        return;
    }
    if (now - state.datachannel_last_received >= AUDIO_TRANSPORT_DATACHANNEL_PROBE_MS && now - state.datachannel_last_probe >= AUDIO_TRANSPORT_DATACHANNEL_PROBE_MS)
    {
        state.datachannel_last_probe = now;
        try
        {
            g_datachannel.send(new Uint8Array(1));
        }
        catch (error)
        {
            // a failed probe remains subject to the silence deadline
        }
    }
}

/**
 * @brief detaches an unresponsive data channel and starts recovery
 *
 * @param string reason -> for the log
 *
 * @return void
 */
function audio_transport__restart_datachannel(reason)
{
    let state = g_audio_transport;

    utils__custom_log("Audio datachannel " + reason + "; rebuilding it");

    g_datachannel = null;
    g_video_datachannel = null;
    g_is_video_datachannel_connected = false;
    g_is_webrtc_datachannel_connected = false;

    if (g_peer_connection_with_server != null)
    {
        try
        {
            g_peer_connection_with_server.close();
        }
        catch (error)
        {
            console.log("closing the dead peer connection failed: " + error.message);
        }
        g_peer_connection_with_server = null;
    }

    if (state.mode === "webrtc")
    {
        state.preferred_down_since = Math.min(state.preferred_down_since, Date.now() - AUDIO_TRANSPORT_FALLBACK_AFTER_MS);
    }

    audio_transport__start_webrtc_if_wanted();
    audio_transport__refresh_ui();
}

/**
 * @brief applies the audio connection preference within the server's allowed transports
 *
 * @return void
 */
function audio_transport__select_mode()
{
    audio_transport__apply_policy_to_ui();
    let previous = g_audio_transport.mode;
    let is_websocket_allowed = (g_server_policy.is_audio_over_websocket_allowed === true);
    let is_webrtc_allowed = (g_server_policy.is_audio_over_webrtc_allowed === true);
    let mode = "none";

    // the preference orders transports. server policy determines whether audio is available
    if (g_audio_connection_preference === "websocket")
    {
        mode = is_websocket_allowed ? "websocket" : (is_webrtc_allowed ? "webrtc" : "none");
    }
    else
    {
        mode = is_webrtc_allowed ? "webrtc" : (is_websocket_allowed ? "websocket" : "none");
    }

    // audio off on the server: no connection is attempted, so nothing asks the server for one
    if (audio_transport__is_audio_on() === false)
    {
        mode = "none";
    }

    g_audio_transport.mode = mode;

    if (mode === previous)
    {
        return;
    }

    // close a WebSocket that is no longer selected or needed as a fallback
    if (g_audio_transport.socket != null && (mode === "none" || (mode === "webrtc" && previous === "websocket")))
    {
        audio_transport__drop_socket();
    }

    g_audio_transport.preferred_down_since = Date.now();
    g_audio_transport.pending_until = 0;
    g_audio_transport.request_attempts = 0;
    audio_transport__start_webrtc_if_wanted();
    audio_transport__refresh_ui();
}

/**
 * @brief stores the user's audio connection preference and applies it
 *
 * @param string preference -> "webrtc" or "websocket"
 *
 * @return void
 */
function audio_transport__set_preference(preference)
{
    if (["webrtc", "websocket"].indexOf(preference) < 0)
    {
        return;
    }
    g_audio_connection_preference = preference;
    utils__storage_set("lemon_audio_connection", preference);
    audio_transport__select_mode();
}

/**
 * @brief shows microphone controls while an audio transport is usable
 *
 * @return void
 */
function audio_transport__refresh_ui()
{
    // the headless node runtime has no webaudio and no microphone controls
    if (typeof window === "undefined" || (typeof window.AudioContext !== "function" && typeof window.webkitAudioContext !== "function") || document.getElementById("toggle-microphone-label") == null)
    {
        return;
    }

    if (audio_transport__ready())
    {
        audio_transport__on_ready();
        return;
    }

    g_is_microphone_available = false;
    voice__update_microphone_button();
    document.getElementById("toggle-microphone-label").style.display = "none";
}

/**
 * @brief reports whether this client is talking, microphone ready or listening only
 *
 * @return void
 */
function audio_transport__report_audio_state()
{
    let value = 3;  // the microphone_usage values: 1 talking, 2 microphone ready, 3 listening only

    if (g_is_authenticated !== true || g_local_client_id < 0 || g_audio_transport.mode === "none")
    {
        return;
    }
    if (g_is_microphone_active === true)
    {
        value = 1;
    }
    else if (g_is_microphone_enabled === true)
    {
        value = 2;
    }
    connection__send_message_object(
    {
        message:
        {
            type: "microphone_usage",
            value: value
        }
    });
}

/**
 * @brief decodes an audio session key with the required byte length
 *
 * @param string text -> hexadecimal key text
 * @param number length -> required decoded byte count
 *
 * @return Uint8Array -> decoded key bytes
 *
 * @throws Error -> the text is not hexadecimal of the required length
 */
function audio_transport__hex_bytes(text, length)
{
    if (typeof text !== "string" || text.length !== length * 2 || !/^[0-9a-f]+$/i.test(text))
    {
        throw new Error("invalid audio session keys");
    }
    let bytes = new Uint8Array(length);
    for (let i = 0; i < length; i++)
    {
        bytes[i] = parseInt(text.substring(i * 2, i * 2 + 2), 16);
    }
    return bytes;
}

/**
 * @brief checks whether either allowed audio connection is ready
 *
 * @return boolean -> true when an active audio transport is available
 */
function audio_transport__ready()
{
    return audio_transport__active() != null;
}

/**
 * @brief detaches callbacks and closes the current audio socket
 *
 * @return void
 */
function audio_transport__drop_socket()
{
    let socket = g_audio_transport.socket;
    g_audio_transport.socket = null;
    g_audio_transport.ready = false;
    if (socket != null)
    {
        socket.onopen = null;
        socket.onmessage = null;
        socket.onclose = null;
        socket.onerror = null;
        socket.close();
    }
}

/**
 * @brief stops audio retries and resets both audio and video sockets
 *
 * @return void
 */
function audio_transport__reset()
{
    if (g_audio_transport.timer != null)
    {
        clearInterval(g_audio_transport.timer);
    }
    g_audio_transport.timer = null;
    g_audio_transport.pending_until = 0;
    g_audio_transport.request_attempts = 0;
    g_audio_transport.preferred_down_since = 0;
    audio_transport__drop_socket();
    if (typeof video_transport__reset === "function")
    {
        video_transport__reset();
    }
}

/**
 * @brief sends an audio packet only while the socket is ready and its queue has room
 *
 * @param ArrayBuffer|Uint8Array data -> packet to send
 *
 * @return boolean -> true when the packet was passed to the socket
 */
function audio_transport__send_packet(data)
{
    let state = g_audio_transport;
    if (!state.ready || state.socket == null || state.socket.readyState !== WebSocket.OPEN)
    {
        return false;
    }
    // the browser owns the TCP queue. do not add old voice behind a stalled send
    if (state.socket.bufferedAmount > 8192)
    {
        return false;
    }
    state.socket.send(data);
    return true;
}

/**
 * @brief encrypts and sends one voice frame through the active audio connection
 *
 * @param Uint8Array sequenced_chunk -> [2 bytes sequence number, least significant first][opus packet]
 *
 * @return void
 */
function audio_transport__send_voice(sequenced_chunk)
{
    let state = g_audio_transport;
    let session_layer = keys__media_session_layer();
    let active = audio_transport__active();

    if (sequenced_chunk == null || sequenced_chunk.byteLength < 2 || sequenced_chunk.byteLength > AUDIO_TRANSPORT_MAX_CONTENT_BYTES || g_is_deep_idle || active == null || session_layer == null || g_current_channel_keys == null || g_local_client_id < 0)
    {
        return;
    }
    if (state.frame_counter == null)
    {
        state.frame_counter = keys__counter_create();
    }
    keys__counter_increment(state.frame_counter);
    let inner = keys__media_apply_inner_layers(g_current_channel_keys, g_local_client_id, state.frame_counter, sequenced_chunk, true);
    inner = keys__media_apply_inner_layers(keys__media_server_key_layers(), g_local_client_id, state.frame_counter, inner, true);
    let frame = keys__media_encrypt_frame(session_layer, MEDIA_STREAM_AUDIO, state.frame_counter, keys__media_build_body(g_local_client_id, state.frame_counter, inner));
    if (frame == null)
    {
        return;
    }
    g_session.bytes_sent += frame.byteLength;
    if (active === "websocket")
    {
        audio_transport__send_packet(frame);
    }
    else
    {
        g_datachannel.send(frame);
    }
}

/**
 * @brief checks connection health and requests a fallback session when its retry window opens
 *
 * @return void
 */
function audio_transport__tick()
{
    let state = g_audio_transport;
    let now = Date.now();

    if (!g_is_authenticated || g_is_deep_idle || state.mode === "none")
    {
        if (state.socket != null)
        {
            audio_transport__drop_socket();
        }
        return;
    }

    if (audio_transport__is_datachannel_open())
    {
        audio_transport__probe_datachannel(now);
    }

    // while the preferred connection is up, the fallback clock stays at now
    if ((state.mode === "webrtc" && audio_transport__is_datachannel_open()) || (state.mode === "websocket" && state.ready))
    {
        state.preferred_down_since = now;
    }

    if (state.ready)
    {
        // a fallback websocket gives way to the preferred datachannel: the server relays over one connection only
        if (state.mode === "webrtc" && audio_transport__is_datachannel_open())
        {
            utils__custom_log("Audio back on the datachannel");
            audio_transport__drop_socket();
            audio_transport__refresh_ui();
            return;
        }
        // keepalive frames refresh last_received. close a socket that stays silent too long
        if (now - state.last_received > 15000)
        {
            audio_transport__drop_socket();
        }
        return;
    }

    // a websocket that does not come up: the datachannel is tried after the fallback time
    audio_transport__start_webrtc_if_wanted();

    if (audio_transport__wants_websocket() === false)
    {
        if (state.socket != null)
        {
            audio_transport__drop_socket();
        }
        return;
    }
    if (now < state.pending_until)
    {
        return;
    }
    if (state.socket != null)
    {
        // an attempt that did not attach within the request window
        audio_transport__drop_socket();
    }
    if (g_local_client_id < 0 || !g_is_client_list_retrieved)
    {
        return;
    }
    // increase the delay after failed attachments. a successful attachment resets it
    state.pending_until = now + Math.min(AUDIO_TRANSPORT_REQUEST_INTERVAL_MS * Math.pow(2, state.request_attempts), AUDIO_TRANSPORT_REQUEST_INTERVAL_MAX_MS);
    state.request_attempts++;
    connection__send_message_object(
    {
        message:
        {
            type: "request_audio_websocket"
        }
    });
}

/**
 * @brief starts audio connection checks and video transport on devices with audio support
 *
 * @return void
 */
function audio_transport__start()
{
    if (typeof window.AudioContext !== "function" && typeof window.webkitAudioContext !== "function")
    {
        return;
    }
    if (g_audio_transport.timer == null)
    {
        g_audio_transport.preferred_down_since = Date.now();
        g_audio_transport.timer = setInterval(audio_transport__tick, 1000);
        audio_transport__tick();
    }
    // the video connection follows the audio one, so it starts and stops with it (video-transport.js)
    if (typeof video_transport__start === "function")
    {
        video_transport__start();
    }
}

/**
 * @brief authenticates a requested audio socket and ignores callbacks from replaced sockets
 *
 * @param object message -> audio session reply with the client ID, ticket and authentication key
 *
 * @return void
 */
function audio_transport__session(message)
{
    let state = g_audio_transport;
    if (state.timer == null || g_is_deep_idle || !g_is_authenticated || state.socket != null || state.mode === "none" || g_server_policy.is_audio_over_websocket_allowed !== true || Date.now() > state.pending_until || message.client_id !== g_local_client_id)
    {
        return;
    }
    try
    {
        let auth_key = audio_transport__hex_bytes(message.auth_key, 32);
        let ticket = audio_transport__hex_bytes(message.ticket, 16);
        let url = new URL(state.url);
        url.pathname = "/audio";
        url.search = "";
        url.hash = "";
        let socket = new WebSocket(url.href);
        state.socket = socket;
        socket.binaryType = "arraybuffer";
        socket.onopen = function()
        {
            if (state.socket !== socket)
            {
                return;
            }
            let hello = new Uint8Array(53);
            hello[0] = 1;
            new DataView(hello.buffer).setUint32(1, message.client_id, false);
            hello.set(ticket, 5);
            hello.set(keys___hmac_sha256(auth_key, hello.subarray(0, 21)), 21);
            socket.send(hello);
        };
        socket.onmessage = function(event)
        {
            if (state.socket !== socket)
            {
                return;
            }
            try
            {
                if (!(event.data instanceof ArrayBuffer))
                {
                    throw new Error("expected binary audio");
                }
                state.last_received = Date.now();
                if (!state.ready)
                {
                    let acknowledgement = new Uint8Array(event.data);
                    if (acknowledgement.length !== 49 || acknowledgement[0] !== 2)
                    {
                        throw new Error("invalid audio attachment acknowledgement");
                    }
                    let tag = keys___hmac_sha256(auth_key, acknowledgement.subarray(0, 17));
                    let difference = 0;
                    for (let i = 0; i < 16; i++)
                    {
                        difference |= ticket[i] ^ acknowledgement[i + 1];
                    }
                    for (let i = 0; i < 32; i++)
                    {
                        difference |= tag[i] ^ acknowledgement[i + 17];
                    }
                    if (difference !== 0)
                    {
                        throw new Error("audio attachment authentication failed");
                    }
                    auth_key.fill(0);
                    state.ready = true;
                    state.pending_until = 0;
                    state.request_attempts = 0;

                    // the preferred datachannel came up while this attached: it carries the audio, the socket goes
                    if (audio_transport__active() !== "websocket")
                    {
                        audio_transport__drop_socket();
                        return;
                    }

                    utils__custom_log("Audio connected through WebSocket");
                    audio_transport__on_ready();
                    if (g_is_microphone_always_on)
                    {
                        voice__activate_microphone();
                    }
                }
                else if (event.data.byteLength > 0)
                {
                    audio_transport__receive(event.data);
                }
                else
                {
                    // the server's keepalive on an idle socket: answered at once, no timer involved
                    audio_transport__send_packet(new Uint8Array(0));
                }
            }
            catch (error)
            {
                utils__custom_log("Audio connection rejected a packet: " + error.message);
                audio_transport__drop_socket();
            }
        };
        socket.onclose = function()
        {
            if (state.socket === socket)
            {
                // retry soon after losing an attached socket. keep the existing delay when attachment never succeeded
                let was_ready = state.ready;
                audio_transport__drop_socket();
                if (was_ready)
                {
                    state.pending_until = Date.now() + 3000;
                }
                utils__custom_log("Audio WebSocket disconnected; reconnecting");
            }
        };
        socket.onerror = function()
        {
            // onclose handles reconnection
        };
    }
    catch (error)
    {
        audio_transport__drop_socket();
        utils__custom_log("Audio connection failed: " + error.message);
    }
}

/**
 * @brief updates microphone controls and reports the local audio state after attachment
 *
 * @return void
 */
function audio_transport__on_ready()
{
    console.log("datachannelonopen");

    let is_client_existing = channel_tree__get_client_index_in_array_by_client_id(g_local_client_id) != -1;

    // not found
    if (is_client_existing == false)
    {
        return;
    }

    let target_element = document.getElementById("client-audio-state-" + g_local_client_id);

    if (g_is_client_running_under_touch_device)
    {
        g_is_microphone_available = true;
        voice__update_microphone_button();
    }
    else
    {
        document.getElementById("toggle-microphone-label").style.display = "block";
    }

    let local_index = channel_tree__get_client_index_in_array_by_client_id(g_local_client_id);

    if (local_index != -1)
    {
        g_client_list[local_index].audio_state = g_is_microphone_active ? G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE : G_AUDIO_STATE.PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;
    }

    if (target_element.classList.contains("client-audio-state-completely-disabled"))
    {
        document.getElementById("client-audio-state-" + g_local_client_id).classList.remove("client-audio-state-completely-disabled");
    }

    document.getElementById("client-audio-state-" + g_local_client_id).classList.add("client-audio-state-microphone-disabled");

    // the server reset this client to listening when the connection attached: tell it the real state
    audio_transport__report_audio_state();
}

/**
 * @brief validates and decrypts a received audio frame before sending it to the decoder
 *
 * @param ArrayBuffer buffer -> the frame as received
 *
 * @return void
 */
function audio_transport__receive(buffer)
{
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength <= MEDIA_FRAME_OVERHEAD)
    {
        return;
    }
    let opened = keys__media_decrypt_frame(keys__media_session_layer(), MEDIA_STREAM_AUDIO, new Uint8Array(buffer));
    if (opened == null)
    {
        return;
    }
    g_session.bytes_received += buffer.byteLength;

    let client_index = channel_tree__get_client_index_in_array_by_client_id(opened.sender_id);
    if (client_index == -1 || opened.sender_id == g_local_client_id)
    {
        return; // not found
    }
    if (g_client_list[client_index].is_muted_by_local_client == true || g_client_list[client_index].is_ignored_by_local_client == true)
    {
        return;
    }
    // audio from a channel we are not in is a stale straggler (e.g. after a switch): discard
    if (g_client_list[client_index].channel_id != g_current_channel_id)
    {
        return;
    }
    let packet_counter = 0n;
    for (const byte of opened.counter_bytes)
    {
        packet_counter = (packet_counter << 8n) | BigInt(byte);
    }
    const discard_through = g_client_list[client_index].discard_audio_through_counter;
    if (discard_through != null && packet_counter <= discard_through)
    {
        return;
    }
    let inner = keys__media_apply_inner_layers(keys__media_server_key_layers(), opened.sender_id, opened.counter_bytes, opened.inner, false);
    let content = null;
    if (g_client_list[client_index].is_music_bot == true)
    {
        content = audio_transport__decrypt_music_bot(g_client_list[client_index], opened.sender_id, opened.counter_bytes, inner);
    }
    else if (g_current_channel_keys != null)
    {
        content = keys__media_apply_inner_layers(g_current_channel_keys, opened.sender_id, opened.counter_bytes, inner, false);
    }
    if (content == null || content.length < 3)
    {
        return;
    }
    let decoded = new Uint8Array(4 + content.length);
    new DataView(decoded.buffer).setInt32(0, opened.sender_id, true);
    decoded.set(content, 4);
    g_opus_decoder_worker.postMessage({
        type: "mainthread__add_decrypted_data_to_opus_decoder",
        counter: packet_counter.toString(),
        value: decoded.buffer
    }, [decoded.buffer]);
}

/**
 * @brief replaces playback keys only for a music bot in the current channel
 *
 * @param object message -> bot ID, channel ID, encryption mode and playback keys
 *
 * @return void
 */
function audio_transport__set_music_bot_key(message)
{
    let index = channel_tree__get_client_index_in_array_by_client_id(message.client_id);
    if (index < 0 || !g_client_list[index].is_music_bot || message.channel_id !== g_current_channel_id || g_client_list[index].channel_id !== message.channel_id)
    {
        return;
    }
    g_client_list[index].music_bot_keys = message.music_bot_keys;
    g_client_list[index].music_bot_encryption = message.music_bot_encryption;
    g_client_list[index].music_bot_key_bytes = null;
}

/**
 * @brief removes the music bot's encryption or requests its missing keys
 *
 * @param object bot -> the bot's entry in g_client_list
 * @param number sender_id -> the bot's client id, from the frame
 * @param Uint8Array counter_bytes -> the bot's counter, from the frame
 * @param Uint8Array inner -> the frame content after the server keys came off
 *
 * @return Uint8Array [2 bytes sequence number][opus], or null without the keys
 */
function audio_transport__decrypt_music_bot(bot, sender_id, counter_bytes, inner)
{
    if (!Array.isArray(bot.music_bot_keys) || bot.music_bot_keys.length !== 4)
    {
        if (!bot.music_bot_key_requested_at || Date.now() - bot.music_bot_key_requested_at > 1000)
        {
            bot.music_bot_key_requested_at = Date.now();
            connection__send_message_object(
            {
                message:
                {
                    type: "request_music_bot_key",
                    client_id: bot.client_id
                }
            });
        }
        return null;
    }
    if (bot.music_bot_key_bytes == null)
    {
        try
        {
            bot.music_bot_key_bytes = bot.music_bot_keys.map(function(key)
            {
                return audio_transport__hex_bytes(key, 32);
            });
        }
        catch (error)
        {
            bot.music_bot_keys = null;
            return null;
        }
    }
    let layers = bot.music_bot_key_bytes.map(function(key)
    {
        return { key_bytes: key, iv_bytes: null };
    });
    return keys__media_apply_inner_layers(layers, sender_id, counter_bytes, inner, false);
}

/**
 * @brief hides audio controls when the server disables audio without changing local preferences
 *
 * @return void
 */
function audio_transport__apply_policy_to_ui()
{
    if (!G_HAS_DOM || !document.body)
    {
        return;
    }
    document.body.classList.toggle("server-audio-disabled", !audio_transport__is_audio_on());
    document.body.classList.toggle("server-voice-disabled", !g_server_policy.is_voice_chat_active);
    document.body.classList.toggle("server-audio-video-disabled", !audio_transport__is_audio_on() && !g_server_policy.is_video_streaming_allowed);
}
