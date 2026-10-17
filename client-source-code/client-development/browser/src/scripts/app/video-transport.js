// why this file exists
// --------------------
// a video stream normally travels over the webrtc data channel. a client that cannot or does not want to
// use webrtc already gets its voice over a second websocket (audio-transport.js); this file gives video the
// same second way: a websocket at /video on the chat port. the server passes a streamer's packets on to
// every viewer over whichever of the two connections that viewer has, so a client on the websocket and a
// client on webrtc see each other's streams.
//
// how the choice is made
// ----------------------
// video follows audio. when this client's audio travels over the websocket right now, or when the server
// does not allow webrtc at all, video uses the video socket too; otherwise it uses the video data channel
// of the webrtc peer connection. the choice is looked at once a second (video_transport__tick). while
// video is in use, that is while this client streams or watches a stream, and the socket is wanted but not
// attached, a session is requested from the server; when the socket is attached but no longer wanted, it
// is closed, and the server then relays over the data channel again. a session request that leads to no
// attached socket is repeated with a growing delay. the socket is attached with the same ticket and proof
// frame as the audio socket (the server handles video connections in video_websocket.c).
//
// what travels over the socket
// ----------------------------
// video packets, each encrypted the same way as a voice frame: with the channel keys, the server keys, and
// a second time with this client's session keys plus an authentication code (keys.js, the block "the
// encryption of audio and video frames"). the video worker (video-worker.js) does that encryption and
// decryption; this file only moves the finished frames. the server sends an empty frame to an idle socket
// every few seconds and this client answers it with an empty frame, like on the audio socket.
var VIDEO_TRANSPORT_REQUEST_INTERVAL_MS = 5000;
var VIDEO_TRANSPORT_REQUEST_INTERVAL_MAX_MS = 60000;
// bytes waiting in the browser's send buffer of the socket above which a packet is dropped instead of queued
var VIDEO_TRANSPORT_SEND_BUFFER_LIMIT = 1024 * 1024;

var g_video_transport =
{
    socket: null,
    ready: false,              // the websocket is attached and acknowledged
    pending_until: 0,          // no new session request before this time
    request_attempts: 0,       // session requests since the last attached socket; sets the delay to the next one
    last_received: 0,          // the server's keepalive frames refresh it; a socket without any frame for 15 s is dead
    timer: null
};

/**
 * @brief whether video should travel over the websocket rather than the webrtc data channel right now
 *
 *        the websocket must be allowed by the server. then: when the server allows no webrtc, the socket is
 *        the only way; when the audio travels over the websocket at the moment, video goes the same way;
 *        when no audio connection is up, the client's preference decides, which is also what audio does
 *
 * @return boolean
 */
function video_transport__prefers_websocket()
{
    if (g_server_policy.is_audio_over_websocket_allowed !== true)
    {
        return false;
    }
    if (g_server_policy.is_audio_over_webrtc_allowed !== true)
    {
        return true;
    }
    let active = audio_transport__active();
    if (active != null)
    {
        return active === "websocket";
    }
    if (g_audio_transport.mode === "none")
    {
        return g_audio_connection_preference === "websocket";
    }
    return g_audio_transport.mode === "websocket";
}

/**
 * @brief whether the webrtc peer connection has to be kept up for video: video is allowed and does not prefer the websocket
 *
 * @return boolean
 */
function video_transport__wants_webrtc()
{
    return g_server_policy.is_video_streaming_allowed === true && video_transport__prefers_websocket() === false;
}

/**
 * @brief whether this client sends or receives video right now: it streams, or it watches a stream
 *
 * @return boolean
 */
function video_transport__is_in_use()
{
    return g_video_stream.role === "streamer" || (g_video_stream.role === "viewer" && g_video_stream.is_watching === true);
}

/**
 * @brief the connection that would carry a video packet right now
 *
 * @return string "websocket", "webrtc" or null
 */
function video_transport__active()
{
    if (g_video_transport.ready && g_video_transport.socket != null && g_video_transport.socket.readyState === WebSocket.OPEN)
    {
        return "websocket";
    }
    if (g_video_datachannel != null && g_video_datachannel.readyState === "open")
    {
        return "webrtc";
    }
    return null;
}

/**
 * @brief how many bytes wait in the send buffer of the active connection; the streamer skips frames when it grows too large
 *
 * @return number bytes, 0 without a connection
 */
function video_transport__buffered_amount()
{
    let active = video_transport__active();
    if (active === "websocket")
    {
        return g_video_transport.socket.bufferedAmount;
    }
    if (active === "webrtc")
    {
        return g_video_datachannel.bufferedAmount;
    }
    return 0;
}

/**
 * @brief sends one finished video frame over the active connection
 *
 * @param ArrayBuffer frame -> the frame, encrypted by the video worker
 *
 * @return boolean true when it was handed to a connection
 */
function video_transport__send(frame)
{
    let active = video_transport__active();
    if (active === "websocket")
    {
        if (g_video_transport.socket.bufferedAmount > VIDEO_TRANSPORT_SEND_BUFFER_LIMIT)
        {
            return false;
        }
        g_video_transport.socket.send(frame);
        return true;
    }
    if (active === "webrtc")
    {
        try
        {
            g_video_datachannel.send(frame);
            return true;
        }
        catch (send_error)
        {
            console.log("video packet not sent: " + send_error.message);
            return false;
        }
    }
    return false;
}

function video_transport__drop_socket()
{
    let socket = g_video_transport.socket;
    g_video_transport.socket = null;
    g_video_transport.ready = false;
    if (socket != null)
    {
        socket.onopen = null;
        socket.onmessage = null;
        socket.onclose = null;
        socket.onerror = null;
        socket.close();
    }
}

function video_transport__reset()
{
    if (g_video_transport.timer != null)
    {
        clearInterval(g_video_transport.timer);
    }
    g_video_transport.timer = null;
    g_video_transport.pending_until = 0;
    g_video_transport.request_attempts = 0;
    video_transport__drop_socket();
}

/**
 * @brief starts the once-a-second check; called when the audio transport starts (audio_transport__start)
 *
 * @return void
 */
function video_transport__start()
{
    if (g_video_transport.timer == null)
    {
        g_video_transport.timer = setInterval(video_transport__tick, 1000);
    }
}

/**
 * @brief the once-a-second check: requests a session when the socket is wanted and missing, drops the socket when it is not wanted or no longer answers
 *
 * @return void
 */
function video_transport__tick()
{
    let state = g_video_transport;
    let now = Date.now();

    if (!g_is_authenticated || g_is_deep_idle || g_server_policy.is_video_streaming_allowed !== true
        || video_transport__is_in_use() === false || video_transport__prefers_websocket() === false)
    {
        if (state.socket != null)
        {
            video_transport__drop_socket();
        }
        return;
    }
    if (state.ready)
    {
        // the server's keepalive frames refresh last_received; a socket without any frame for this long no longer works
        if (now - state.last_received > 15000)
        {
            video_transport__drop_socket();
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
        video_transport__drop_socket();
    }
    if (g_local_client_id < 0 || !g_is_client_list_retrieved)
    {
        return;
    }
    // a request that leads nowhere is repeated with a growing delay; an attached socket resets the delay
    state.pending_until = now + Math.min(VIDEO_TRANSPORT_REQUEST_INTERVAL_MS * Math.pow(2, state.request_attempts), VIDEO_TRANSPORT_REQUEST_INTERVAL_MAX_MS);
    state.request_attempts++;
    connection__send_message_object(
    {
        message:
        {
            type: "request_video_websocket"
        }
    });
}

/**
 * @brief the server issued a video session (the message video_websocket_session): the socket is opened and attached with the proof frame
 *
 *        the proof frame is [1][4 bytes client id][16 bytes ticket][32 bytes HMAC-SHA256 with the session key
 *        over the first 21 bytes]; the server answers with [2][16 bytes ticket][32 bytes HMAC-SHA256 over the
 *        first 17 bytes], which is checked here, so the socket talks to the server that issued the session.
 *        after that every frame from the socket is a video packet for video__on_media_frame, and an empty
 *        frame is the server's keepalive, answered at once
 *
 * @param object message -> client_id, auth_key (hex), ticket (hex)
 *
 * @return void
 */
function video_transport__session(message)
{
    let state = g_video_transport;
    if (state.timer == null || g_is_deep_idle || !g_is_authenticated || state.socket != null
        || g_server_policy.is_audio_over_websocket_allowed !== true
        || Date.now() > state.pending_until || message.client_id !== g_local_client_id)
    {
        return;
    }
    try
    {
        let auth_key = audio_transport__hex_bytes(message.auth_key, 32);
        let ticket = audio_transport__hex_bytes(message.ticket, 16);
        let url = new URL(g_audio_transport.url);
        url.pathname = "/video";
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
                    throw new Error("expected binary video");
                }
                state.last_received = Date.now();
                if (!state.ready)
                {
                    let acknowledgement = new Uint8Array(event.data);
                    if (acknowledgement.length !== 49 || acknowledgement[0] !== 2)
                    {
                        throw new Error("invalid video attachment acknowledgement");
                    }
                    let code = keys___hmac_sha256(auth_key, acknowledgement.subarray(0, 17));
                    let difference = 0;
                    for (let i = 0; i < 16; i++)
                    {
                        difference |= ticket[i] ^ acknowledgement[i + 1];
                    }
                    for (let i = 0; i < 32; i++)
                    {
                        difference |= code[i] ^ acknowledgement[i + 17];
                    }
                    if (difference !== 0)
                    {
                        throw new Error("video attachment authentication failed");
                    }
                    auth_key.fill(0);
                    state.ready = true;
                    state.pending_until = 0;
                    state.request_attempts = 0;
                    // Losing the previous socket clears the server's watch flag. Restore current
                    // viewing intent only after this attachment has been authenticated.
                    if (g_video_stream.role === "viewer" && g_video_stream.is_watching === true)
                    {
                        connection__send_message_object(
                        {
                            message:
                            {
                                type: "video_stream_watch",
                                is_watching: true
                            }
                        });
                        video__request_keyframe();
                    }
                    utils__custom_log("Video connected through WebSocket");
                }
                else if (event.data.byteLength > 0)
                {
                    video__on_media_frame(event.data);
                }
                else
                {
                    // the server's keepalive on an idle socket: answered at once
                    socket.send(new Uint8Array(0));
                }
            }
            catch (error)
            {
                utils__custom_log("Video connection rejected a packet: " + error.message);
                video_transport__drop_socket();
            }
        };
        socket.onclose = function()
        {
            if (state.socket === socket)
            {
                // a socket that was attached is requested again soon; one that never attached keeps the delay its request set
                let was_ready = state.ready;
                video_transport__drop_socket();
                if (was_ready)
                {
                    state.pending_until = Date.now() + 3000;
                }
                utils__custom_log("Video WebSocket disconnected");
            }
        };
        socket.onerror = function()
        {
            // onclose handles the retry
        };
    }
    catch (error)
    {
        video_transport__drop_socket();
        utils__custom_log("Video connection failed: " + error.message);
    }
}
