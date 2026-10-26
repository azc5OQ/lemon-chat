// voice travels either over the webrtc datachannel or over a second websocket on the chat port.
// mode is the connection this client prefers, "webrtc" or "websocket", from the user's preference
// (g_audio_connection_preference) within what the server allows (g_server_policy); "none" only when
// the server serves no audio connection at all, which is the server's decision, never the client's.
// when the preferred connection has not come up for AUDIO_TRANSPORT_FALLBACK_AFTER_MS, the other
// allowed one is tried as well, and whichever is up carries the audio (audio_transport__active).
// once the preferred one comes up again it takes over. the chat connection negotiates a websocket
// session; audio packets then use the /audio socket.
// the server sends an empty frame to an idle audio socket every few seconds and this client answers it,
// so no timer is needed to keep the socket. a session request that leads to no attached socket is
// repeated with a growing delay, from AUDIO_TRANSPORT_REQUEST_INTERVAL_MS up to the maximum below.
// after every attach the client reports its audio state (talking, microphone ready or listening),
// because the server resets that state when a connection attaches.
// the data channel is probed with a one-byte frame, which the server echoes, after a few seconds of
// silence; when nothing comes back for AUDIO_TRANSPORT_DATACHANNEL_DEAD_MS the channel is rebuilt, and
// the websocket is tried at once, instead of waiting for the browser to notice the lost transport.
// every frame on either connection is encrypted a second time with this client's two session keys and
// carries an authentication code, so that a frame on a plain websocket can neither be read nor forged on
// the network path; keys.js (the block "the encryption of audio and video frames") and media_auth.c on
// the server explain it. the voice itself stays under the channel keys, which the server does not have.
var AUDIO_TRANSPORT_FALLBACK_AFTER_MS = 10000;
var AUDIO_TRANSPORT_REQUEST_INTERVAL_MS = 10000;
var AUDIO_TRANSPORT_REQUEST_INTERVAL_MAX_MS = 60000;
var AUDIO_TRANSPORT_DATACHANNEL_PROBE_MS = 5000;
var AUDIO_TRANSPORT_DATACHANNEL_DEAD_MS = 15000;
// the largest frame content; with the 29 bytes the encryption adds, a frame stays under the server's limit of 4132 bytes
var AUDIO_TRANSPORT_MAX_CONTENT_BYTES = 4080;

var g_audio_transport =
{
    mode: "webrtc",
    url: "",
    socket: null,
    ready: false,              // the websocket is attached and authenticated
    pending_until: 0,          // no new session request before this time
    request_attempts: 0,       // session requests since the last attached socket; sets the delay to the next one
    preferred_down_since: 0,   // when the preferred connection was last up, or when the attempt began
    last_received: 0,
    datachannel_last_received: 0, // the last frame of any kind from the data channel
    datachannel_last_probe: 0,
    frame_counter: null,       // counts the frames this client sent; part of every counter block of a frame's encryption (keys.js)
    timer: null
};

/**
 * @brief whether the server has audio on at all: voice or music bot audio
 *
 * @return boolean
 */
function audio_transport__is_audio_on()
{
    return g_server_policy.is_voice_chat_active === true || g_server_policy.is_music_bot_audio_active === true;
}

function audio_transport__is_datachannel_open()
{
    return g_datachannel != null && g_datachannel.readyState === "open";
}

/**
 * @brief whether the preferred connection has been down long enough for the other one to be tried
 *
 * @return boolean
 */
function audio_transport__is_fallback_due()
{
    return Date.now() - g_audio_transport.preferred_down_since >= AUDIO_TRANSPORT_FALLBACK_AFTER_MS;
}

/**
 * @brief the connection that carries the audio right now
 *
 *        the preferred one whenever it is up, otherwise the other one when it is up and the server
 *        allows voice over it (the fallback time only decides when the other one is established).
 *        the server relays to a client over its websocket whenever one is attached, so a fallback
 *        websocket is dropped as soon as the preferred datachannel is back (see audio_transport__tick).
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
 * @brief whether the datachannel should be kept up: voice over webrtc, the fallback of a websocket that does not come up, or video streaming
 *
 * @return boolean
 */
function audio_transport__wants_webrtc()
{
    let state = g_audio_transport;

    // video keeps the peer connection only while it does not prefer the video websocket (video-transport.js)
    if (g_server_policy.is_video_streaming_allowed === true
        && (typeof video_transport__prefers_websocket !== "function" || video_transport__prefers_websocket() === false))
    {
        return true;
    }

    if (state.mode === "webrtc")
    {
        return true;
    }

    return state.mode === "websocket" && g_server_policy.is_audio_over_webrtc_allowed === true
        && state.ready === false && audio_transport__is_fallback_due();
}

/**
 * @brief whether a websocket session should be requested: voice over websocket, or the fallback of a datachannel that does not come up
 *
 * @return boolean
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
 * @brief starts the datachannel connection loop when the datachannel is wanted and no loop runs
 *
 * @return void
 */
function audio_transport__start_webrtc_if_wanted()
{
    // nothing to start while the data channel is up or being set up: the loop would only log and leave
    if (g_is_webrtc_datachannel_connected === true || audio_transport__is_datachannel_open())
    {
        return;
    }
    if (audio_transport__wants_webrtc() === false || g_is_webrtc_datachannel_check_running == true
        || g_is_deep_idle == true || g_is_authenticated == false || g_is_voice_chat_allowed_by_server != true
        || typeof RTCPeerConnection === "undefined")
    {
        return;
    }

    g_is_webrtc_datachannel_check_running = true;
    voice__webrtc_datachannel_connection_check(true);
}

/**
 * @brief the datachannel opened: a fallback websocket gives way to it, and the microphone controls follow
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
 * @brief a frame from the data channel: an answer to a probe (one byte) or audio, which is played
 *
 *        the server relays to this client over one connection at a time, its choice, so whatever
 *        arrives here is played
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
 * @brief keeps watch over an open data channel: a probe after a quiet spell, a rebuild when nothing comes back
 *
 *        a transport whose udp path died stays "open" in the browser until its consent checks time out,
 *        which takes about half a minute; the probe shortens that to AUDIO_TRANSPORT_DATACHANNEL_DEAD_MS
 *
 * @param number now -> Date.now()
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
            // a channel that refuses the probe is as good as silent; the silence check above takes it from here
        }
    }
}

/**
 * @brief drops a data channel the server no longer answers on and starts the connection loop for a new one
 *
 *        the lost channel is detached first, so its late close event cannot touch the new state. in webrtc
 *        mode the websocket fallback is due at once: the silence limit already passed.
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
 * @brief picks the audio connection from the user's preference and the server's policy, and applies a change
 *
 *        the preference names the first choice; the other allowed connection is the second. a
 *        websocket that the new choice no longer wants is dropped; the tick and the datachannel
 *        callbacks then establish the chosen one.
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

    // the preference only orders the two connections; whether audio is served at all is the server's call
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

    // no audio at all, or back to the datachannel by choice: the websocket goes. a fallback socket
    // that is now the chosen connection stays
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
 * @brief shows or hides the microphone controls according to whether an audio connection is usable now
 *
 * @return void
 */
function audio_transport__refresh_ui()
{
    // the headless node runtime has no webaudio and no microphone controls
    if (typeof window === "undefined" || (typeof window.AudioContext !== "function" && typeof window.webkitAudioContext !== "function")
        || document.getElementById("toggle-microphone-label") == null)
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
 * @brief reports this client's audio state to the server: talking, microphone ready, or listening only
 *
 *        the server sets a client to "listening" when an audio connection attaches and to "audio disabled"
 *        when its last one drops, so the current state is reported after every attach: a push-to-talk key
 *        that is still held keeps talking across a handover
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

function audio_transport__ready()
{
    return audio_transport__active() != null;
}

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

function audio_transport__send_packet(data)
{
    let state = g_audio_transport;
    if (!state.ready || state.socket == null || state.socket.readyState !== WebSocket.OPEN)
    {
        return false;
    }
    // The browser owns the TCP queue. Do not add old voice behind a stalled send.
    if (state.socket.bufferedAmount > 8192)
    {
        return false;
    }
    state.socket.send(data);
    return true;
}

/**
 * @brief sends one voice frame of this client over the connection that carries the audio right now
 *
 *        the frame content ([2 bytes sequence number][opus]) is encrypted with the channel keys, then with
 *        the server keys, and then a second time with this client's session keys plus an authentication code
 *        (keys.js explains the layers). every frame gets the next value of the frame counter, so no two
 *        frames are ever encrypted with the same counter block
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

    if (sequenced_chunk == null || sequenced_chunk.byteLength < 2 || sequenced_chunk.byteLength > AUDIO_TRANSPORT_MAX_CONTENT_BYTES
        || g_is_deep_idle || active == null || session_layer == null || g_current_channel_keys == null || g_local_client_id < 0)
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
        // the server's keepalive frames refresh last_received; a socket without any frame for this long no longer works
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
    // a request that leads nowhere is repeated with a growing delay, so a blocked /audio path costs the
    // server one session per minute at most; an attached socket resets the delay
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

function audio_transport__session(message)
{
    let state = g_audio_transport;
    if (state.timer == null || g_is_deep_idle || !g_is_authenticated || state.socket != null
        || state.mode === "none" || g_server_policy.is_audio_over_websocket_allowed !== true
        || Date.now() > state.pending_until || message.client_id !== g_local_client_id)
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
                // a socket that was attached is requested again soon; one that never attached keeps the delay its request set
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
            // onclose handles reconnection.
        };
    }
    catch (error)
    {
        audio_transport__drop_socket();
        utils__custom_log("Audio connection failed: " + error.message);
    }
}

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
        g_client_list[local_index].audio_state = g_is_microphone_active
            ? G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE : G_AUDIO_STATE.PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;
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
 * @brief a frame that arrived over the websocket or the data channel: checked, decrypted and handed to the decoder worker
 *
 *        first the authentication code is checked and the server's encryption removed (keys.js); that gives the
 *        sender id and the sender counter. a sender that is muted, ignored, in another channel, or this client
 *        itself is dropped. then the server keys come off, and either the music bot's keys (a bot) or the
 *        channel keys (a member). what is left, [2 bytes sequence number][opus], goes to the decoder worker
 *        behind the sender id
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
        value: decoded.buffer
    }, [decoded.buffer]);
}

function audio_transport__set_music_bot_key(message)
{
    let index = channel_tree__get_client_index_in_array_by_client_id(message.client_id);
    if (index < 0 || !g_client_list[index].is_music_bot
        || message.channel_id !== g_current_channel_id || g_client_list[index].channel_id !== message.channel_id)
    {
        return;
    }
    g_client_list[index].music_bot_keys = message.music_bot_keys;
    g_client_list[index].music_bot_encryption = message.music_bot_encryption;
    g_client_list[index].music_bot_key_bytes = null;
}

/**
 * @brief removes a music bot's own encryption from a frame content
 *
 *        the server encrypts every bot frame with the bot's four keys under the counter block of the bot id
 *        and the bot's counter (media_auth.c). the four keys reach the client over the encrypted chat
 *        connection; a client that does not have them yet asks for them, once per second at most
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
 * @brief Hides audio controls when the server disables audio without changing local preferences.
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
