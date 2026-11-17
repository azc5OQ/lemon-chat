// carries video through /video when audio uses WebSocket or server policy disallows WebRTC
// otherwise video uses the peer connection data channel. the server can relay between both transports
// while streaming or viewing, request missing sessions with increasing retry delays
// attachment uses the same ticket and proof format as audio. echo empty keepalives
// the video worker encrypts and decrypts frames. this module only moves them
var VIDEO_TRANSPORT_REQUEST_INTERVAL_MS = 5000;
var VIDEO_TRANSPORT_REQUEST_INTERVAL_MAX_MS = 60000;
// bytes waiting in the browser's send buffer of the socket above which a packet is dropped instead of queued
var VIDEO_TRANSPORT_SEND_BUFFER_LIMIT = 1024 * 1024;

var g_video_transport =
{
    socket: null,
    ready: false,              // the websocket is attached and acknowledged
    pending_until: 0,          // no new session request before this time
    request_attempts: 0,       // failed attachment count used to calculate the next retry delay
    last_received: 0,          // last received frame time. keepalives count toward the silence deadline
    timer: null
};

/**
 * @brief chooses video transport from server policy and the current audio preference
 *
 * @return boolean true when the allowed video transport should use a WebSocket
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
 * @brief checks whether video still requires the WebRTC connection
 *
 * @return boolean true when video is allowed and does not prefer a WebSocket
 */
function video_transport__wants_webrtc()
{
    return g_server_policy.is_video_streaming_allowed === true && video_transport__prefers_websocket() === false;
}

/**
 * @brief checks whether this client is streaming or watching
 *
 * @return boolean true while streaming or actively watching
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
 * @brief how many bytes wait in the send buffer of the active connection. the streamer skips frames when it grows too large
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
    if (video_transport__buffered_amount() + frame.byteLength > VIDEO_TRANSPORT_SEND_BUFFER_LIMIT)
    {
        return false;
    }
    try
    {
        if (active === "websocket")
        {
            g_video_transport.socket.send(frame);
            return true;
        }
        if (active === "webrtc")
        {
            g_video_datachannel.send(frame);
            return true;
        }
    }
    catch (send_error)
    {
        return false;
    }
    return false;
}

/**
 * @brief detaches callbacks and closes the current video socket
 *
 * @return void
 */
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

/**
 * @brief stops video retries and clears the current socket
 *
 * @return void
 */
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
 * @brief starts the periodic video transport check
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
 * @brief requests needed sessions and closes unwanted or unresponsive video sockets
 *
 * @return void
 */
function video_transport__tick()
{
    let state = g_video_transport;
    let now = Date.now();

    if (!g_is_authenticated || g_is_deep_idle || g_server_policy.is_video_streaming_allowed !== true || video_transport__is_in_use() === false || video_transport__prefers_websocket() === false)
    {
        if (state.socket != null)
        {
            video_transport__drop_socket();
        }
        return;
    }
    if (state.ready)
    {
        // close the socket when no frame or keepalive arrives before the deadline
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
    // increase failed attachment delays. reset the delay after a successful attachment
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
 * @brief opens the issued video socket and verifies its attachment proof
 *
 * @param object message -> client_id, auth_key (hex), ticket (hex)
 *
 * @return void
 */
function video_transport__session(message)
{
    let state = g_video_transport;
    if (state.timer == null || g_is_deep_idle || !g_is_authenticated || state.socket != null || g_server_policy.is_audio_over_websocket_allowed !== true || Date.now() > state.pending_until || message.client_id !== g_local_client_id)
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
                    // losing the previous socket clears the server's watch flag. restore current
                    // viewing intent only after this attachment has been authenticated
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
                // retry soon after losing an attached socket. keep the existing delay when attachment never succeeded
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
