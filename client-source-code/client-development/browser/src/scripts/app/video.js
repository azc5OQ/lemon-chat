// video.js is embedded in template.html along with the other client files
// it is the video streaming side of the page: a member streams its screen or a video file to the
// channel it sits in, one stream per channel, and every other member opts in. frames are encoded with
// webcodecs, packetized and encrypted with the channel keys in the video worker (video-worker.js),
// and travel on the second, reliable datachannel the server opens next to the audio one. the server
// relays them blindly. the full design is in VIDEO_STREAMING_DESIGN.md at the repo root
// messages.js hands the six server messages in here, dispatch.js the worker's answers, ui.js the datachannel

// ---- constants private to this file ----

// the encrypted packet plus the 4 byte sender id the server prepends must stay under the 16 KiB every
// browser is happy to receive on a datachannel
var VIDEO_PACKET_MAX_BYTES = 15000;

// keyframe every this often, so a viewer that connects or lost frames waits at most this long
var VIDEO_KEYFRAME_INTERVAL_MS = 2000;

// above this much queued on the datachannel the streamer skips frames; a long run of skips halves the bitrate
var VIDEO_SEND_BUFFER_HIGH_WATER_BYTES = 512 * 1024;
var VIDEO_SKIPS_BEFORE_BITRATE_STEP = 30;

// a viewer asks for a keyframe at most this often, the server rate limits per streamer on top
var VIDEO_KEYFRAME_REQUEST_MIN_INTERVAL_MS = 1000;

// one conservative preset per source, no selector yet (see the design notes)
var VIDEO_PRESETS = {
    screen: { max_edge: 1280, fps: 15, bitrate: 1500000, content_hint: "detail" },
    file: { max_edge: 1280, fps: 30, bitrate: 2500000, content_hint: "motion" }
};

// h.264 first (hardware nearly everywhere, annex b keeps the parameter sets in-band with every
// keyframe so a viewer can join on any of them), vp8 as the software fallback
var VIDEO_CODEC_CANDIDATES = [
    { codec: "avc1.42E01E", avc: { format: "annexb" } },
    { codec: "vp8" }
];

// ---- state private to this file (the shared state object is g_video_stream in globals.js) ----

var video_source_media_stream = null;      // the getDisplayMedia stream while sharing the screen
var video_source_object_url = null;        // the blob url of the file while streaming a file
var video_encoder = null;
var video_encoder_config = null;
var video_scale_canvas = null;             // a file larger than the cap is drawn down through this
var video_scale_context = null;
var video_frame_number = 0;
var video_last_keyframe_ms = 0;
var video_force_keyframe = false;
var video_last_encoded_ms = 0;
var video_skip_streak = 0;
var video_current_bitrate = 0;
var video_pump_timer = null;
var video_pump_is_rvfc = false;
var video_is_pumping = false;
var video_last_keyframe_request_sent_ms = 0;
var video_is_worker_canvas_transferred = false;
var video_popup_drag = null;               // { start_x, start_y, left, top } while the header is dragged

/**
 * @brief whether this browser can take part: webcodecs on both ends, plus an offscreen canvas for the viewer's worker
 *
 * @return boolean true when streaming and viewing can work here
 */
function video__is_supported()
{
    return typeof VideoEncoder === "function" && typeof VideoDecoder === "function"
        && typeof HTMLCanvasElement !== "undefined" && typeof HTMLCanvasElement.prototype.transferControlToOffscreen === "function";
}

/**
 * @brief the channel object of the channel this client sits in, or null
 *
 * @return object|null the channel
 */
function video__get_current_channel()
{
    for (let i = 0; i < g_channel_list.length; i++)
    {
        if (g_channel_list[i].channel_id == g_current_channel_id)
        {
            return g_channel_list[i];
        }
    }
    return null;
}

/**
 * @brief the username of a client id, for the popup's texts
 *
 * @param number client_id -> the client
 *
 * @return string the name, or "someone"
 */
function video__get_username(client_id)
{
    let index = channel_tree__get_client_index_in_array_by_client_id(client_id);

    if (index == -1)
    {
        return "someone";
    }

    return chat__sanitize_string(g_client_list[index].username);
}

/**
 * @brief the server-wide policy arrived or changed: with video streaming off nothing stream related exists in the page; a stream running while it flips off is torn down
 *
 * @return void
 */
function video__apply_policy_to_ui()
{
    if (typeof document === "undefined")
    {
        return;
    }

    if (g_server_policy.is_video_streaming_allowed != true)
    {
        video__teardown_all("disabled");
        video__close_popup(false);
    }

    video__refresh_controls();
}

/**
 * @brief shows or hides every stream control from the current state: the chat row button, the channel properties row, the tree icons and the popup's buttons
 *        the rule is "nothing visible unless the server allows it", then "only what makes sense now"
 *
 * @return void
 */
function video__refresh_controls()
{
    if (typeof document === "undefined")
    {
        return;
    }

    let is_allowed = (g_server_policy.is_video_streaming_allowed == true);
    let channel = video__get_current_channel();
    let is_channel_enabled = (channel != null && channel.is_video_stream_enabled != false);
    let button_container = document.getElementById("upload-video-stream-container");
    let button = document.getElementById("video-stream-button");
    let properties_label = document.getElementById("channel-properties-disable-video-label");
    let properties_field = document.getElementById("channel-properties-disable-video-field");

    // the channel properties row exists only while the server allows streaming at all
    if (properties_label != null) { properties_label.style.display = is_allowed ? "" : "none"; }
    if (properties_field != null) { properties_field.style.display = is_allowed ? "" : "none"; }

    if (button_container != null && button != null)
    {
        let is_someone_else_streaming = (g_video_stream.is_active == true && g_video_stream.streamer_client_id != g_local_client_id);
        let show = is_allowed && is_channel_enabled && is_someone_else_streaming == false;

        button_container.style.display = show ? "" : "none";

        if (video__is_supported() == false)
        {
            button.classList.add("chat-file-uploads-off");
            button.title = "video streaming needs a browser with webcodecs (chrome / edge 94, safari 16.4, firefox 132 or newer)";
        }
        else if (g_video_stream.role == "streamer")
        {
            button.classList.remove("chat-file-uploads-off");
            button.title = "you are streaming - open the stream window";
        }
        else
        {
            button.classList.remove("chat-file-uploads-off");
            button.title = "stream your screen or a video file to this channel";
        }
    }

    video__refresh_tree_icons();
    video__refresh_popup_buttons();
}

/**
 * @brief the stream icon on the streamer's row in the channel tree: shown on the one row that streams in this channel, hidden everywhere else
 *
 * @return void
 */
function video__refresh_tree_icons()
{
    let icons = document.getElementsByClassName("client-video-stream-icon");

    for (let i = 0; i < icons.length; i++)
    {
        let icon_client_id = parseInt(icons[i].getAttribute("data-client-id"));
        let show = (g_server_policy.is_video_streaming_allowed == true && g_video_stream.is_active == true && icon_client_id == g_video_stream.streamer_client_id);

        icons[i].style.display = show ? "inline-block" : "none";
    }
}

/**
 * @brief wires the buttons and the popup once at startup; called from main.js after the dom exists
 *
 * @return void
 */
function video__init_ui()
{
    if (typeof document === "undefined")
    {
        return;
    }

    let button = document.getElementById("video-stream-button");
    let file_input = document.getElementById("choose-video-stream-file-input");
    let choice_screen = document.getElementById("video-stream-choice-screen");
    let choice_file = document.getElementById("video-stream-choice-file");
    let choice_cancel = document.getElementById("video-stream-choice-cancel");
    let popup_header = document.getElementById("video-stream-popup-header");
    let popup_close = document.getElementById("video-stream-popup-close");

    if (button != null) { button.onclick = video__stream_button_onclick; }
    if (file_input != null) { file_input.onchange = video__file_input_onchange; }
    if (choice_screen != null) { choice_screen.onclick = function() { video__hide_choice_dialog(); video__start_screen_share(); }; }
    if (choice_file != null) { choice_file.onclick = function() { video__hide_choice_dialog(); if (file_input != null) { file_input.click(); } }; }
    if (choice_cancel != null) { choice_cancel.onclick = video__hide_choice_dialog; }
    if (popup_close != null) { popup_close.onclick = function() { video__close_popup(true); }; }

    document.getElementById("video-stream-connect-button").onclick = video__connect;
    document.getElementById("video-stream-disconnect-button").onclick = function() { video__disconnect(true); };
    document.getElementById("video-stream-stop-button").onclick = function() { video__stop_stream(true, "stopped"); };
    document.getElementById("video-stream-fullscreen-button").onclick = video__toggle_fullscreen;

    // the header drags the popup; resizing is css (resize: both on the popup)
    if (popup_header != null)
    {
        popup_header.addEventListener("pointerdown", video__popup_drag_start);
        document.addEventListener("pointermove", video__popup_drag_move);
        document.addEventListener("pointerup", video__popup_drag_end);
    }

    // the stream icon on a tree row is re-rendered with the row, so the click is delegated
    document.addEventListener("click", function(event)
    {
        if (event.target != null && event.target.classList != null && event.target.classList.contains("client-video-stream-icon"))
        {
            event.stopPropagation();
            video__open_popup();
        }
    }, true);

    // the viewer's canvas is handed to the worker once; the worker draws every stream into it
    let canvas = document.getElementById("video-stream-canvas");

    if (canvas != null && g_video_worker != null && video__is_supported() == true && video_is_worker_canvas_transferred == false)
    {
        try
        {
            let offscreen = canvas.transferControlToOffscreen();
            g_video_worker.postMessage({ type: "mainthread__video_worker_init", canvas: offscreen }, [offscreen]);
            video_is_worker_canvas_transferred = true;
        }
        catch (transfer_error)
        {
            console.log("video: could not hand the canvas to the worker: " + transfer_error.message);
        }
    }

    video__refresh_controls();
}

/**
 * @brief the chat row button: opens the "screen or file" choice, or the popup when this client already streams
 *
 * @return void
 */
function video__stream_button_onclick()
{
    if (g_server_policy.is_video_streaming_allowed != true)
    {
        return;
    }

    if (video__is_supported() == false)
    {
        utils__custom_alert("video streaming needs a browser with webcodecs (chrome / edge 94, safari 16.4, firefox 132 or newer)");
        return;
    }

    if (g_video_stream.role == "streamer")
    {
        video__open_popup();
        return;
    }

    if (g_video_stream.is_active == true)
    {
        utils__custom_alert(video__get_username(g_video_stream.streamer_client_id) + " is already streaming in this channel");
        return;
    }

    video__show_choice_dialog();
}

/**
 * @brief shows the "share screen or share a video file" choice
 *
 * @return void
 */
function video__show_choice_dialog()
{
    document.getElementById("video-stream-choice-container").style.display = "block";
}

/**
 * @brief hides the choice
 *
 * @return void
 */
function video__hide_choice_dialog()
{
    document.getElementById("video-stream-choice-container").style.display = "none";
}

/**
 * @brief asks the browser for the screen (its own picker), then starts the stream with that media stream
 *        the browser's own "stop sharing" button ends the track, which ends the stream
 *
 * @return void
 */
function video__start_screen_share()
{
    if (navigator.mediaDevices == null || typeof navigator.mediaDevices.getDisplayMedia != "function")
    {
        utils__custom_alert("screen sharing needs a secure connection: open the client over HTTPS, from localhost, or as a local file");
        return;
    }

    let preset = VIDEO_PRESETS.screen;
    let constraints = {
        video: { width: { max: preset.max_edge }, height: { max: preset.max_edge }, frameRate: { max: preset.fps } },
        audio: false
    };

    navigator.mediaDevices.getDisplayMedia(constraints).then(function(stream)
    {
        let tracks = stream.getVideoTracks();

        if (tracks.length == 0)
        {
            utils__custom_alert("the browser gave no video track to share");
            return;
        }

        try { tracks[0].contentHint = preset.content_hint; } catch (hint_error) { }

        tracks[0].onended = function()
        {
            if (g_video_stream.role == "streamer" || g_video_stream.is_start_pending == true)
            {
                video__stop_stream(true, "stopped");
            }
        };

        video_source_media_stream = stream;
        video__begin_stream("screen", stream, null);
    }, function(error)
    {
        console.log("getDisplayMedia refused: " + error.message);
    });
}

/**
 * @brief the file picker's answer: a video file from disk becomes the stream's source
 *
 * @param object event -> the change event
 *
 * @return void
 */
function video__file_input_onchange(event)
{
    let files = event.target.files;

    if (files == null || files.length == 0)
    {
        return;
    }

    if (video_source_object_url != null)
    {
        URL.revokeObjectURL(video_source_object_url);
        video_source_object_url = null;
    }

    video_source_object_url = URL.createObjectURL(files[0]);
    video__begin_stream("file", null, video_source_object_url);
}

/**
 * @brief common start for both sources: the hidden preview video element gets the source, its dimensions decide the encoded size, the codec is probed, and the server is asked to start
 *        encoding begins only when the server confirms with video_stream_state (see video__on_stream_confirmed)
 *
 * @param string source -> "screen" or "file"
 * @param MediaStream media_stream -> the screen's stream, or null
 * @param string object_url -> the file's blob url, or null
 *
 * @return void
 */
function video__begin_stream(source, media_stream, object_url)
{
    let preview = document.getElementById("video-stream-preview");
    let preset = VIDEO_PRESETS[source];

    if (g_video_stream.is_active == true && g_video_stream.streamer_client_id != g_local_client_id)
    {
        video__release_source();
        utils__custom_alert("somebody else started streaming in the meantime");
        return;
    }

    preview.muted = true;
    preview.loop = false;

    if (media_stream != null)
    {
        preview.src = "";
        preview.srcObject = media_stream;
    }
    else
    {
        preview.srcObject = null;
        preview.src = object_url;
    }

    g_video_stream.is_start_pending = true;
    g_video_stream.source = source;
    video__open_popup();
    video__set_popup_status("preparing the stream ...");

    preview.onloadedmetadata = function()
    {
        preview.onloadedmetadata = null;

        let source_width = preview.videoWidth;
        let source_height = preview.videoHeight;

        if (source_width == 0 || source_height == 0)
        {
            video__stop_stream(false, "stopped");
            utils__custom_alert("the browser could not read the video's size");
            return;
        }

        // cap the longest edge, keep the aspect, even sizes keep every encoder happy
        let scale = Math.min(1, preset.max_edge / Math.max(source_width, source_height));
        let width = Math.max(16, Math.floor(source_width * scale / 2) * 2);
        let height = Math.max(16, Math.floor(source_height * scale / 2) * 2);

        video__pick_codec(width, height, preset).then(function(config)
        {
            if (config == null)
            {
                video__stop_stream(false, "stopped");
                utils__custom_alert("this browser has no video encoder webcodecs can use");
                return;
            }

            if (g_video_stream.is_start_pending != true)
            {
                return; // cancelled while probing
            }

            video_encoder_config = config;
            g_video_stream.codec = config.codec;
            g_video_stream.width = width;
            g_video_stream.height = height;
            g_video_stream.fps = preset.fps;

            connection__send_message_object({
                message: {
                    type: "video_stream_start",
                    source: source,
                    codec: config.codec,
                    width: width,
                    height: height,
                    fps: preset.fps
                }
            });

            video__set_popup_status("asking the server ...");
        });

        preview.play().catch(function(play_error) { console.log("preview play: " + play_error.message); });
    };

    preview.onerror = function()
    {
        video__stop_stream(false, "stopped");
        utils__custom_alert("the browser cannot play this file");
    };

    preview.onended = function()
    {
        // a file played to its end: the stream ends with it
        if (g_video_stream.role == "streamer" && g_video_stream.source == "file")
        {
            video__stop_stream(true, "stopped");
        }
    };
}

/**
 * @brief finds the first codec of VIDEO_CODEC_CANDIDATES this browser can encode at the given size
 *
 * @param number width -> encoded width
 * @param number height -> encoded height
 * @param object preset -> the source's preset (fps, bitrate)
 *
 * @return Promise resolves with the usable encoder config, or null
 */
function video__pick_codec(width, height, preset)
{
    let candidates = VIDEO_CODEC_CANDIDATES.slice();

    function try_next()
    {
        if (candidates.length == 0)
        {
            return Promise.resolve(null);
        }

        let candidate = candidates.shift();
        let config = {
            codec: candidate.codec,
            width: width,
            height: height,
            bitrate: preset.bitrate,
            framerate: preset.fps,
            latencyMode: "realtime"
        };

        if (candidate.avc != null)
        {
            config.avc = candidate.avc;
        }

        return VideoEncoder.isConfigSupported(config).then(function(result)
        {
            if (result != null && result.supported == true)
            {
                return config;
            }
            return try_next();
        }, function()
        {
            return try_next();
        });
    }

    return try_next();
}

/**
 * @brief the server confirmed our stream (video_stream_state with our id): the encoder is configured and the frame pump starts
 *
 * @return void
 */
function video__on_stream_confirmed()
{
    let preview = document.getElementById("video-stream-preview");

    g_video_stream.is_start_pending = false;
    g_video_stream.role = "streamer";
    g_video_stream.viewer_count = 0;
    g_video_stream.epoch = (g_video_stream.epoch + 1) & 0xff;
    video_frame_number = 0;
    video_last_keyframe_ms = 0;
    video_force_keyframe = true;
    video_skip_streak = 0;
    video_current_bitrate = video_encoder_config.bitrate;

    try
    {
        video_encoder = new VideoEncoder({
            output: video__on_encoded_chunk,
            error: function(error)
            {
                console.log("video encoder error: " + error.message);
                video__stop_stream(true, "stopped");
                utils__custom_alert("the video encoder failed: " + error.message);
            }
        });
        video_encoder.configure(video_encoder_config);
    }
    catch (encoder_error)
    {
        console.log("video encoder setup failed: " + encoder_error.message);
        video__stop_stream(true, "stopped");
        utils__custom_alert("the video encoder could not start: " + encoder_error.message);
        return;
    }

    preview.style.display = "block";
    document.getElementById("video-stream-canvas").style.display = "none";
    video__set_popup_status("");
    video__set_popup_title("you are streaming your " + (g_video_stream.source == "screen" ? "screen" : "video"));

    video_is_pumping = true;
    video__schedule_next_frame();
    video__refresh_controls();
}

/**
 * @brief arms the next frame pull: the video element's own frame callback where it exists, a timer at the target rate otherwise
 *
 * @return void
 */
function video__schedule_next_frame()
{
    let preview = document.getElementById("video-stream-preview");

    if (video_is_pumping == false)
    {
        return;
    }

    if (typeof preview.requestVideoFrameCallback === "function")
    {
        video_pump_is_rvfc = true;
        preview.requestVideoFrameCallback(video__pump_frame);
    }
    else
    {
        video_pump_is_rvfc = false;
        video_pump_timer = setTimeout(function() { video__pump_frame(performance.now(), null); }, 1000 / g_video_stream.fps);
    }
}

/**
 * @brief one frame: rate limited to the preset, skipped under backpressure, scaled through the canvas when the source is larger than the cap, then handed to the encoder
 *
 * @param number now -> the callback's time in ms
 * @param object metadata -> the frame metadata from requestVideoFrameCallback, or null
 *
 * @return void
 */
function video__pump_frame(now, metadata)
{
    let preview = document.getElementById("video-stream-preview");
    let frame = null;
    let min_interval_ms = 1000 / g_video_stream.fps;

    if (video_is_pumping == false || video_encoder == null || video_encoder.state != "configured")
    {
        return;
    }

    // the element's callback fires per composited frame (60 a second on a screen): keep to the preset rate
    if (video_pump_is_rvfc == true && now - video_last_encoded_ms < min_interval_ms - 2)
    {
        video__schedule_next_frame();
        return;
    }

    // backpressure: a full send queue means the link cannot keep up; skip, and after a run of skips
    // halve the bitrate (stepped back up slowly once it drains)
    if (g_video_datachannel != null && g_video_datachannel.bufferedAmount > VIDEO_SEND_BUFFER_HIGH_WATER_BYTES)
    {
        video_skip_streak++;

        if (video_skip_streak >= VIDEO_SKIPS_BEFORE_BITRATE_STEP && video_current_bitrate > 200000)
        {
            video_skip_streak = 0;
            video_current_bitrate = Math.floor(video_current_bitrate / 2);
            video__reconfigure_bitrate(video_current_bitrate);
            console.log("video: link is slow, bitrate down to " + video_current_bitrate);
        }

        video__schedule_next_frame();
        return;
    }

    if (video_skip_streak == 0 && video_current_bitrate < video_encoder_config.bitrate && video_frame_number % 150 == 0 && video_frame_number > 0)
    {
        video_current_bitrate = Math.min(video_encoder_config.bitrate, Math.floor(video_current_bitrate * 1.25));
        video__reconfigure_bitrate(video_current_bitrate);
    }

    video_skip_streak = 0;

    // pending queue in the encoder: never let it pile up, drop instead
    if (video_encoder.encodeQueueSize > 2)
    {
        video__schedule_next_frame();
        return;
    }

    try
    {
        let timestamp_us = Math.round(now * 1000);

        if (preview.videoWidth != g_video_stream.width || preview.videoHeight != g_video_stream.height)
        {
            if (video_scale_canvas == null)
            {
                video_scale_canvas = document.createElement("canvas");
                video_scale_context = video_scale_canvas.getContext("2d", { alpha: false });
            }

            if (video_scale_canvas.width != g_video_stream.width || video_scale_canvas.height != g_video_stream.height)
            {
                video_scale_canvas.width = g_video_stream.width;
                video_scale_canvas.height = g_video_stream.height;
            }

            video_scale_context.drawImage(preview, 0, 0, g_video_stream.width, g_video_stream.height);
            frame = new VideoFrame(video_scale_canvas, { timestamp: timestamp_us });
        }
        else
        {
            frame = new VideoFrame(preview, { timestamp: timestamp_us });
        }

        let want_keyframe = video_force_keyframe || (now - video_last_keyframe_ms >= VIDEO_KEYFRAME_INTERVAL_MS);

        video_encoder.encode(frame, { keyFrame: want_keyframe });
        frame.close();

        if (want_keyframe)
        {
            video_last_keyframe_ms = now;
            video_force_keyframe = false;
        }

        video_last_encoded_ms = now;
    }
    catch (frame_error)
    {
        if (frame != null) { try { frame.close(); } catch (close_error) { } }
        console.log("video frame skipped: " + frame_error.message);
    }

    video__schedule_next_frame();
}

/**
 * @brief re-configures the running encoder with a new bitrate, keeping everything else
 *
 * @param number bitrate -> bits per second
 *
 * @return void
 */
function video__reconfigure_bitrate(bitrate)
{
    if (video_encoder == null || video_encoder.state != "configured")
    {
        return;
    }

    try
    {
        let config = Object.assign({}, video_encoder_config);
        config.bitrate = bitrate;
        video_encoder.configure(config);
        video_force_keyframe = true;
    }
    catch (reconfigure_error)
    {
        console.log("video bitrate change failed: " + reconfigure_error.message);
    }
}

/**
 * @brief the encoder produced a chunk: its bytes go to the worker, which packetizes and encrypts them with the channel keys and posts the packets back for sending
 *
 * @param EncodedVideoChunk chunk -> the encoded frame
 * @param object metadata -> encoder metadata (unused, annex b needs no description)
 *
 * @return void
 */
function video__on_encoded_chunk(chunk, metadata)
{
    if (g_video_stream.role != "streamer" || g_video_worker == null)
    {
        return;
    }

    let data = new ArrayBuffer(chunk.byteLength);
    chunk.copyTo(data);

    video_frame_number = (video_frame_number + 1) & 0xffff;

    g_video_worker.postMessage({
        type: "mainthread__video_worker_packetize",
        epoch: g_video_stream.epoch,
        frame_number: video_frame_number,
        is_keyframe: (chunk.type == "key"),
        timestamp: chunk.timestamp,
        data: data
    }, [data]);
}

/**
 * @brief the worker's encrypted packets for one frame: sent on the video datachannel in order
 *
 * @param array packets -> ArrayBuffers
 *
 * @return void
 */
function video__send_packets(packets)
{
    if (g_video_stream.role != "streamer" || g_video_datachannel == null || g_video_datachannel.readyState != "open")
    {
        return;
    }

    for (let i = 0; i < packets.length; i++)
    {
        try
        {
            g_video_datachannel.send(packets[i]);
            g_session.bytes_sent += packets[i].byteLength;
        }
        catch (send_error)
        {
            console.log("video packet not sent: " + send_error.message);
            return;
        }
    }
}

/**
 * @brief ends this client's stream: the pump, the encoder and the source are released, the server is told when asked, the popup says so
 *
 * @param boolean tell_server -> send video_stream_stop
 * @param string reason -> what the popup shows
 *
 * @return void
 */
function video__stop_stream(tell_server, reason)
{
    let was_streaming = (g_video_stream.role == "streamer" || g_video_stream.is_start_pending == true);

    video_is_pumping = false;

    if (video_pump_timer != null)
    {
        clearTimeout(video_pump_timer);
        video_pump_timer = null;
    }

    if (video_encoder != null)
    {
        try { video_encoder.close(); } catch (close_error) { }
        video_encoder = null;
    }

    video__release_source();

    if (tell_server == true && was_streaming == true && g_is_websocket_connected == true)
    {
        connection__send_message_object({ message: { type: "video_stream_stop" } });
    }

    g_video_stream.is_start_pending = false;

    if (g_video_stream.role == "streamer")
    {
        g_video_stream.role = "none";
        g_video_stream.is_active = false;
        g_video_stream.streamer_client_id = -1;
        g_video_stream.viewer_count = 0;
        g_video_stream.pending_viewer_requests = [];
        video__render_pending_requests();
        video__set_popup_status("stream ended (" + reason + ")");
        video__set_popup_title("stream");
    }

    document.getElementById("video-stream-preview").style.display = "none";
    video__refresh_controls();
}

/**
 * @brief lets go of the screen stream or the file url and clears the preview element
 *
 * @return void
 */
function video__release_source()
{
    let preview = document.getElementById("video-stream-preview");

    if (video_source_media_stream != null)
    {
        let tracks = video_source_media_stream.getTracks();
        for (let i = 0; i < tracks.length; i++)
        {
            tracks[i].onended = null;
            try { tracks[i].stop(); } catch (stop_error) { }
        }
        video_source_media_stream = null;
    }

    if (video_source_object_url != null)
    {
        URL.revokeObjectURL(video_source_object_url);
        video_source_object_url = null;
    }

    if (preview != null)
    {
        preview.onloadedmetadata = null;
        preview.onerror = null;
        preview.onended = null;
        try { preview.pause(); } catch (pause_error) { }
        preview.srcObject = null;
        preview.removeAttribute("src");
        try { preview.load(); } catch (load_error) { }
    }
}

// ---- viewer side ----

/**
 * @brief the server offered the channel's stream to us: the popup opens with a connect button, unless we ignore the streamer
 *
 * @param object msg -> the server message (streamer_client_id, source, codec, width, height, fps)
 *
 * @return void
 */
function video__on_offer(msg)
{
    if (typeof document === "undefined")
    {
        return;
    }

    let streamer_index =channel_tree__get_client_index_in_array_by_client_id(msg.message.streamer_client_id);

    if (streamer_index != -1 && g_client_list[streamer_index].is_ignored_by_local_client == true)
    {
        return;
    }

    g_video_stream.is_active = true;
    g_video_stream.streamer_client_id = msg.message.streamer_client_id;
    g_video_stream.source = msg.message.source;
    g_video_stream.codec = msg.message.codec;
    g_video_stream.width = msg.message.width;
    g_video_stream.height = msg.message.height;
    g_video_stream.fps = msg.message.fps;
    g_video_stream.role = "viewer";
    g_video_stream.is_offered = true;
    g_video_stream.is_watching = false;

    video__open_popup();
    video__set_popup_title(video__get_username(msg.message.streamer_client_id) + " is streaming " + (msg.message.source == "screen" ? "their screen" : "a video"));

    if (video__is_supported() == false)
    {
        video__set_popup_status("this browser cannot show the stream (no webcodecs)");
        g_video_stream.is_decoder_supported = false;
        video__refresh_controls();
        return;
    }

    g_video_stream.is_decoder_supported = false;
    video__set_popup_status("checking the codec ...");

    VideoDecoder.isConfigSupported({ codec: msg.message.codec, codedWidth: msg.message.width, codedHeight: msg.message.height }).then(function(result)
    {
        g_video_stream.is_decoder_supported = (result != null && result.supported == true);
        video__set_popup_status(g_video_stream.is_decoder_supported ? "press connect to watch" : "this browser cannot decode " + msg.message.codec);
        video__refresh_controls();
    }, function()
    {
        g_video_stream.is_decoder_supported = false;
        video__set_popup_status("this browser cannot decode " + msg.message.codec);
        video__refresh_controls();
    });

    video__refresh_controls();
}

/**
 * @brief the connect button: the worker's decoder is set up for the announced codec and the server is asked to relay to us
 *
 * @return void
 */
function video__connect()
{
    if (g_video_stream.role != "viewer" || g_video_stream.is_offered != true || g_video_stream.is_decoder_supported != true)
    {
        return;
    }

    if (g_video_worker == null || video_is_worker_canvas_transferred == false)
    {
        video__set_popup_status("the viewer is not available in this browser");
        return;
    }

    g_video_worker.postMessage({
        type: "mainthread__video_worker_decoder_setup",
        codec: g_video_stream.codec,
        width: g_video_stream.width,
        height: g_video_stream.height
    });

    video__post_channel_keys_to_worker();

    connection__send_message_object({ message: { type: "video_stream_watch", is_watching: true } });

    g_video_stream.is_watching = true;
    document.getElementById("video-stream-canvas").style.display = "block";
    document.getElementById("video-stream-preview").style.display = "none";
    video__set_popup_status("connecting ...");
    video__refresh_controls();
}

/**
 * @brief the disconnect button (and close): the server stops relaying to us and the decoder is reset
 *
 * @param boolean tell_server -> send video_stream_watch false
 *
 * @return void
 */
function video__disconnect(tell_server)
{
    if (g_video_stream.is_watching == true && tell_server == true && g_is_websocket_connected == true)
    {
        connection__send_message_object({ message: { type: "video_stream_watch", is_watching: false } });
    }

    g_video_stream.is_watching = false;

    if (g_video_worker != null)
    {
        g_video_worker.postMessage({ type: "mainthread__video_worker_decoder_reset" });
    }

    if (g_video_stream.role == "viewer")
    {
        video__set_popup_status("disconnected - press connect to watch again");
    }

    video__refresh_controls();
}

/**
 * @brief the video datachannel arrived from the server (label "video"): stored and wired; ui.js calls this from the datachannel event
 *
 * @param RTCDataChannel channel -> the channel
 *
 * @return void
 */
function video__on_datachannel_received(channel)
{
    g_video_datachannel = channel;
    channel.binaryType = "arraybuffer";

    channel.onopen = function()
    {
        g_is_video_datachannel_connected = true;
        console.log("video datachannel open");
    };

    channel.onclose = function(event)
    {
        if (event != null && event.target !== g_video_datachannel)
        {
            return;
        }

        g_is_video_datachannel_connected = false;
        console.log("video datachannel closed");

        // the server drops a watching viewer whose transport died; mirror it here so connect works again
        if (g_video_stream.is_watching == true)
        {
            video__disconnect(false);
        }
    };

    channel.onmessage = video__on_datachannel_message;
}

/**
 * @brief a packet from the video datachannel: [4B sender id][encrypted packet]; only the channel's streamer is decoded, and only while we watch
 *
 * @param MessageEvent event -> event.data is the ArrayBuffer
 *
 * @return void
 */
function video__on_datachannel_message(event)
{
    if (event.data == null || event.data.byteLength <= 4)
    {
        return;
    }

    g_session.bytes_received += event.data.byteLength;

    if (g_video_stream.role != "viewer" || g_video_stream.is_watching != true || g_video_worker == null)
    {
        return;
    }

    let sender_client_id = new DataView(event.data).getInt32(0, true);

    if (sender_client_id != g_video_stream.streamer_client_id)
    {
        return;
    }

    let payload = event.data.slice(4);

    g_video_worker.postMessage({ type: "mainthread__video_worker_decode_packet", value: payload }, [payload]);
}

/**
 * @brief the worker lost frames (a gap, a decrypt failure, a decoder error): the streamer is asked for a keyframe, rate limited
 *
 * @return void
 */
function video__request_keyframe()
{
    let now = performance.now();

    if (g_video_stream.role != "viewer" || g_video_stream.is_watching != true)
    {
        return;
    }

    if (now - video_last_keyframe_request_sent_ms < VIDEO_KEYFRAME_REQUEST_MIN_INTERVAL_MS)
    {
        return;
    }

    video_last_keyframe_request_sent_ms = now;
    connection__send_message_object({ message: { type: "video_stream_keyframe_request" } });
}

/**
 * @brief every worker answer lands here from dispatch.js
 *
 * @param object data -> the posted message
 *
 * @return void
 */
function video__on_worker_message(data)
{
    if (data.type == "video_worker__packets_ready")
    {
        if (data.epoch == g_video_stream.epoch)
        {
            video__send_packets(data.packets);
        }
    }
    else if (data.type == "video_worker__keyframe_needed")
    {
        video__request_keyframe();
    }
    else if (data.type == "video_worker__first_frame")
    {
        if (g_video_stream.is_watching == true)
        {
            video__set_popup_status("");
        }
    }
    else if (data.type == "video_worker__decoder_error")
    {
        console.log("video decoder error: " + data.value);
        video__request_keyframe();
    }
    else if (data.type == "video_worker__log")
    {
        console.log("video worker: " + data.value);
    }
}

/**
 * @brief the channel keys changed (a new set from the maintainer): the worker encrypts and decrypts with them from now on; a streamer sends a keyframe so viewers resync at once
 *
 * @return void
 */
function video__post_channel_keys_to_worker()
{
    if (g_video_worker == null || g_current_channel_keys == null)
    {
        return;
    }

    g_video_worker.postMessage({
        type: "mainthread__channel_keys_for_video_worker",
        value: g_current_channel_keys
    });

    if (g_video_stream.role == "streamer")
    {
        video_force_keyframe = true;
    }
}

// ---- protocol handlers (messages.js calls these) ----

/**
 * @brief video_stream_state: a stream started or ended in a channel. ours: the encoder starts. somebody else's: the icon appears and the popup waits for the offer. an end tears everything down
 *
 * @param object msg -> the server message
 *
 * @return void
 */
function video__on_state(msg)
{
    // the headless node runtime (android) has no page: video lives in the webview only
    if (typeof document === "undefined")
    {
        return;
    }

    if (msg.message.channel_id != g_current_channel_id)
    {
        return;
    }

    if (msg.message.is_active == true)
    {
        g_video_stream.is_active = true;
        g_video_stream.streamer_client_id = msg.message.streamer_client_id;
        g_video_stream.source = msg.message.source;
        g_video_stream.codec = msg.message.codec;
        g_video_stream.width = msg.message.width;
        g_video_stream.height = msg.message.height;
        g_video_stream.fps = msg.message.fps;

        if (msg.message.streamer_client_id == g_local_client_id)
        {
            if (g_video_stream.is_start_pending == true)
            {
                video__on_stream_confirmed();
            }
        }
        else
        {
            if (g_video_stream.role == "streamer" || g_video_stream.is_start_pending == true)
            {
                // we lost the race for the channel's one stream
                video__stop_stream(false, "somebody else is streaming");
            }
            g_video_stream.role = "viewer";
            g_video_stream.is_offered = false;
            g_video_stream.is_watching = false;
        }
    }
    else
    {
        let reason = (msg.message.reason != null) ? msg.message.reason.replace(/_/g, " ") : "ended";

        if (g_video_stream.role == "streamer" || g_video_stream.is_start_pending == true)
        {
            video__stop_stream(false, reason);
        }
        else
        {
            video__teardown_viewer(reason);
        }

        g_video_stream.is_active = false;
        g_video_stream.streamer_client_id = -1;
    }

    video__refresh_controls();
}

/**
 * @brief the viewer side forgets the stream; the popup stays if open and says why
 *
 * @param string reason -> the text
 *
 * @return void
 */
function video__teardown_viewer(reason)
{
    if (g_video_stream.is_watching == true)
    {
        video__disconnect(false);
    }

    g_video_stream.role = "none";
    g_video_stream.is_offered = false;
    g_video_stream.is_watching = false;
    g_video_stream.is_decoder_supported = false;

    document.getElementById("video-stream-canvas").style.display = "none";

    if (g_video_stream.is_popup_open == true)
    {
        video__set_popup_status("stream ended (" + reason + ")");
        video__set_popup_title("stream");
    }
}

/**
 * @brief everything, both roles: used on disconnect, on a channel switch and when the server switches the feature off
 *
 * @param string reason -> the text for an open popup
 *
 * @return void
 */
function video__teardown_all(reason)
{
    if (typeof document === "undefined")
    {
        return;
    }

    if (g_video_stream.role == "streamer" || g_video_stream.is_start_pending == true)
    {
        video__stop_stream(false, reason);
    }

    video__teardown_viewer(reason);

    g_video_stream.is_active = false;
    g_video_stream.streamer_client_id = -1;
    g_video_stream.viewer_count = 0;
    g_video_stream.pending_viewer_requests = [];
    video__render_pending_requests();
    video__refresh_controls();
}

/**
 * @brief video_stream_viewer_request: somebody joined our streaming channel, the popup asks whether they may watch
 *
 * @param object msg -> the server message (client_id, username)
 *
 * @return void
 */
function video__on_viewer_request(msg)
{
    if (typeof document === "undefined" || g_video_stream.role != "streamer")
    {
        return;
    }

    for (let i = 0; i < g_video_stream.pending_viewer_requests.length; i++)
    {
        if (g_video_stream.pending_viewer_requests[i].client_id == msg.message.client_id)
        {
            return;
        }
    }

    g_video_stream.pending_viewer_requests.push({ client_id: msg.message.client_id, username: msg.message.username });
    video__open_popup();
    video__render_pending_requests();
}

/**
 * @brief the streamer's yes / no for a pending viewer
 *
 * @param number client_id -> the member
 * @param boolean is_allowed -> the answer
 *
 * @return void
 */
function video__answer_viewer_request(client_id, is_allowed)
{
    connection__send_message_object({ message: { type: "video_stream_allow_viewer", client_id: client_id, is_allowed: is_allowed } });

    g_video_stream.pending_viewer_requests = g_video_stream.pending_viewer_requests.filter(function(entry) { return entry.client_id != client_id; });
    video__render_pending_requests();
}

/**
 * @brief draws the "allow x to watch?" strip of the streamer's popup from the pending list
 *
 * @return void
 */
function video__render_pending_requests()
{
    let container = document.getElementById("video-stream-popup-requests");

    if (container == null)
    {
        return;
    }

    container.innerHTML = "";

    for (let i = 0; i < g_video_stream.pending_viewer_requests.length; i++)
    {
        let entry = g_video_stream.pending_viewer_requests[i];
        let row = document.createElement("div");
        let text = document.createElement("span");
        let yes = document.createElement("input");
        let no = document.createElement("input");

        row.className = "video-stream-request-row";
        text.textContent = "allow " + entry.username + " to watch?";
        yes.type = "button";
        yes.value = "yes";
        yes.onclick = (function(id) { return function() { video__answer_viewer_request(id, true); }; })(entry.client_id);
        no.type = "button";
        no.value = "no";
        no.onclick = (function(id) { return function() { video__answer_viewer_request(id, false); }; })(entry.client_id);

        row.appendChild(text);
        row.appendChild(yes);
        row.appendChild(no);
        container.appendChild(row);
    }

    container.style.display = (g_video_stream.pending_viewer_requests.length > 0) ? "block" : "none";
}

/**
 * @brief video_stream_viewer_state: a viewer connected or dropped; the count follows and a new viewer gets a keyframe
 *
 * @param object msg -> the server message (client_id, is_watching)
 *
 * @return void
 */
function video__on_viewer_state(msg)
{
    if (typeof document === "undefined" || g_video_stream.role != "streamer")
    {
        return;
    }

    if (msg.message.is_watching == true)
    {
        g_video_stream.viewer_count++;
        video_force_keyframe = true;
    }
    else if (g_video_stream.viewer_count > 0)
    {
        g_video_stream.viewer_count--;
    }

    video__refresh_popup_buttons();
}

/**
 * @brief video_stream_keyframe_request: a viewer lost frames, the next frame is a keyframe
 *
 * @return void
 */
function video__on_keyframe_request()
{
    if (g_video_stream.role == "streamer")
    {
        video_force_keyframe = true;
    }
}

/**
 * @brief video_stream_refused: the server said no to a start or a connect, with a short reason
 *
 * @param object msg -> the server message (reason)
 *
 * @return void
 */
function video__on_refused(msg)
{
    if (typeof document === "undefined")
    {
        return;
    }

    let texts = {
        disabled: "video streaming is switched off on this server",
        channel_disabled: "video streaming is switched off in this channel",
        busy: "somebody else is already streaming in this channel",
        not_allowed: "the streamer has not allowed you to watch",
        no_stream: "there is no stream in this channel",
        revoked: "the streamer stopped your access to the stream"
    };
    let text = (texts[msg.message.reason] != null) ? texts[msg.message.reason] : ("refused: " + msg.message.reason);

    if (g_video_stream.is_start_pending == true)
    {
        video__stop_stream(false, text);
    }

    if (g_video_stream.is_watching == true && msg.message.reason != "busy")
    {
        video__disconnect(false);
    }

    if (msg.message.reason == "revoked")
    {
        g_video_stream.is_offered = false;
    }

    if (g_video_stream.is_popup_open == true)
    {
        video__set_popup_status(text);
    }
    else
    {
        utils__custom_alert(text);
    }

    video__refresh_controls();
}

/**
 * @brief the client list arrived (login, refresh): a stream already running in our channel shows its icon; the offer, if we may watch, comes separately
 *
 * @param array clients -> the raw list entries from the server
 *
 * @return void
 */
function video__apply_client_list(clients)
{
    if (typeof document === "undefined" || clients == null)
    {
        return;
    }

    for (let i = 0; i < clients.length; i++)
    {
        if (clients[i].is_streaming_video == true && clients[i].channel_id == g_current_channel_id)
        {
            g_video_stream.is_active = true;
            g_video_stream.streamer_client_id = clients[i].client_id;

            if (clients[i].client_id != g_local_client_id && g_video_stream.role != "viewer")
            {
                g_video_stream.role = "viewer";
                g_video_stream.is_offered = false;
                g_video_stream.is_watching = false;
            }
        }
    }

    video__refresh_controls();
}

/**
 * @brief this client moved to another channel: the old channel's stream is gone for us (the server ended ours if we streamed)
 *
 * @return void
 */
function video__on_local_channel_changed()
{
    if (typeof document === "undefined")
    {
        return;
    }

    video__teardown_all("you left the channel");
    video__close_popup(false);
}

/**
 * @brief the connection dropped: everything is forgotten
 *
 * @return void
 */
function video__reset_on_disconnect()
{
    if (typeof document === "undefined")
    {
        return;
    }

    g_video_datachannel = null;
    g_is_video_datachannel_connected = false;
    video__teardown_all("disconnected");
    video__close_popup(false);
}

// ---- the popup ----

/**
 * @brief shows the popup (the streamer's preview or a viewer's picture)
 *
 * @return void
 */
function video__open_popup()
{
    let popup = document.getElementById("video-stream-popup");

    if (popup == null || g_server_policy.is_video_streaming_allowed != true)
    {
        return;
    }

    popup.style.display = "flex";
    g_video_stream.is_popup_open = true;

    if (g_video_stream.role == "viewer" && g_video_stream.is_offered == true && g_video_stream.is_watching == false)
    {
        video__set_popup_status(g_video_stream.is_decoder_supported ? "press connect to watch" : "checking the codec ...");
    }
    else if (g_video_stream.role == "viewer" && g_video_stream.is_offered != true)
    {
        video__set_popup_title(video__get_username(g_video_stream.streamer_client_id) + " is streaming");
        video__set_popup_status("waiting for the streamer to let you watch ...");
    }

    video__refresh_popup_buttons();
}

/**
 * @brief hides the popup; a watching viewer disconnects with it (a hidden popup must cost nothing), a streamer keeps streaming and reopens it from the tree icon
 *
 * @param boolean is_user_action -> true from the close button
 *
 * @return void
 */
function video__close_popup(is_user_action)
{
    if (typeof document === "undefined")
    {
        return;
    }

    let popup = document.getElementById("video-stream-popup");

    if (popup == null)
    {
        return;
    }

    if (is_user_action == true && g_video_stream.role == "viewer" && g_video_stream.is_watching == true)
    {
        video__disconnect(true);
    }

    if (document.fullscreenElement != null && document.fullscreenElement.id == "video-stream-popup")
    {
        document.exitFullscreen().catch(function() { });
    }

    popup.style.display = "none";
    g_video_stream.is_popup_open = false;
}

/**
 * @brief which of the popup's buttons apply right now
 *
 * @return void
 */
function video__refresh_popup_buttons()
{
    let connect = document.getElementById("video-stream-connect-button");
    let disconnect = document.getElementById("video-stream-disconnect-button");
    let stop = document.getElementById("video-stream-stop-button");
    let count = document.getElementById("video-stream-viewer-count");

    if (connect == null)
    {
        return;
    }

    let is_viewer = (g_video_stream.role == "viewer" && g_video_stream.is_offered == true);

    connect.style.display = (is_viewer && g_video_stream.is_watching == false) ? "" : "none";
    connect.disabled = (g_video_stream.is_decoder_supported != true);
    disconnect.style.display = (is_viewer && g_video_stream.is_watching == true) ? "" : "none";
    stop.style.display = (g_video_stream.role == "streamer" || g_video_stream.is_start_pending == true) ? "" : "none";

    if (g_video_stream.role == "streamer")
    {
        count.textContent = g_video_stream.viewer_count + (g_video_stream.viewer_count == 1 ? " viewer" : " viewers");
    }
    else
    {
        count.textContent = "";
    }
}

/**
 * @brief the popup's status line
 *
 * @param string text -> the text, empty hides it
 *
 * @return void
 */
function video__set_popup_status(text)
{
    let status = document.getElementById("video-stream-popup-status");

    if (status != null)
    {
        status.textContent = text;
        status.style.display = (text.length > 0) ? "block" : "none";
    }
}

/**
 * @brief the popup's title
 *
 * @param string text -> the text
 *
 * @return void
 */
function video__set_popup_title(text)
{
    let title = document.getElementById("video-stream-popup-title");

    if (title != null)
    {
        title.textContent = text;
    }
}

/**
 * @brief fullscreen on the popup's body (the picture fills the screen, the buttons stay reachable with escape)
 *
 * @return void
 */
function video__toggle_fullscreen()
{
    let popup = document.getElementById("video-stream-popup");

    if (document.fullscreenElement != null)
    {
        document.exitFullscreen().catch(function() { });
        return;
    }

    if (popup != null && typeof popup.requestFullscreen === "function")
    {
        popup.requestFullscreen().catch(function(error) { console.log("fullscreen refused: " + error.message); });
    }
}

/**
 * @brief drag start on the popup header
 *
 * @param PointerEvent event -> the event
 *
 * @return void
 */
function video__popup_drag_start(event)
{
    let popup = document.getElementById("video-stream-popup");

    if (event.target != null && event.target.id == "video-stream-popup-close")
    {
        return;
    }

    let rect = popup.getBoundingClientRect();

    video_popup_drag = { start_x: event.clientX, start_y: event.clientY, left: rect.left, top: rect.top };

    // once dragged the popup is pinned by left/top instead of right/bottom
    popup.style.left = rect.left + "px";
    popup.style.top = rect.top + "px";
    popup.style.right = "auto";
    popup.style.bottom = "auto";

    event.preventDefault();
}

/**
 * @brief drag move
 *
 * @param PointerEvent event -> the event
 *
 * @return void
 */
function video__popup_drag_move(event)
{
    if (video_popup_drag == null)
    {
        return;
    }

    let popup = document.getElementById("video-stream-popup");
    let left = video_popup_drag.left + (event.clientX - video_popup_drag.start_x);
    let top = video_popup_drag.top + (event.clientY - video_popup_drag.start_y);

    left = Math.max(0, Math.min(window.innerWidth - 80, left));
    top = Math.max(0, Math.min(window.innerHeight - 40, top));

    popup.style.left = left + "px";
    popup.style.top = top + "px";
}

/**
 * @brief drag end
 *
 * @return void
 */
function video__popup_drag_end()
{
    video_popup_drag = null;
}
