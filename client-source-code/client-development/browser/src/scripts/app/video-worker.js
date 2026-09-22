// video-worker.js is embedded in template.html along with the other client files
// it runs inside the "video_worker" web worker only (see worker-entry.js). streamer side: it splits an
// encoded frame into packets and encrypts each with the channel keys. viewer side: it decrypts and
// reassembles packets into frames, decodes them with webcodecs and draws them on the popup's canvas,
// which the main thread handed over as an OffscreenCanvas. the main thread only forwards buffers
//
// packet layout inside the encryption (12 byte header, little endian):
//   [1 B stream epoch][2 B frame number][2 B packet index][2 B packet count][1 B flags][4 B timestamp, low 32 bits of the chunk's microseconds]
//   flags: bit0 = keyframe, bit1 = last packet of the frame

var VIDEO_WORKER_HEADER_BYTES = 12;
var VIDEO_WORKER_MAX_PACKETS_PER_FRAME = 256;

var video_worker_channel_keys = null;
var video_worker_canvas = null;
var video_worker_context = null;
var video_worker_decoder = null;
var video_worker_decoder_codec = "";
var video_worker_waiting_for_keyframe = true;
var video_worker_assembly = null;          // the frame being reassembled: { epoch, frame_number, packet_count, received, parts, is_keyframe, timestamp }
var video_worker_epoch = -1;
var video_worker_has_drawn = false;
var video_worker_last_keyframe_request_ms = 0;

/**
 * @brief the video worker's message handler (see worker-entry.js)
 *
 * @param object e -> the worker message event
 *
 * @return void
 */
function video_worker__onmessage(e)
{
    let data = e.data;

    if (data.type == "mainthread__channel_keys_for_video_worker")
    {
        video_worker_channel_keys = data.value;
    }
    else if (data.type == "mainthread__video_worker_packetize")
    {
        video_worker__packetize(data);
    }
    else if (data.type == "mainthread__video_worker_init")
    {
        video_worker_canvas = data.canvas;
        video_worker_context = video_worker_canvas.getContext("2d", { alpha: false });
    }
    else if (data.type == "mainthread__video_worker_decoder_setup")
    {
        video_worker__decoder_setup(data.codec, data.width, data.height);
    }
    else if (data.type == "mainthread__video_worker_decoder_reset")
    {
        video_worker__decoder_close();
        video_worker__clear_canvas();
    }
    else if (data.type == "mainthread__video_worker_decode_packet")
    {
        video_worker__decode_packet(data.value);
    }
}

/**
 * @brief splits one encoded frame into packets under the size cap, encrypts each with the channel keys and posts them back for sending
 *
 * @param object data -> epoch, frame_number, is_keyframe, timestamp and the frame bytes (ArrayBuffer)
 *
 * @return void
 */
function video_worker__packetize(data)
{
    if (video_worker_channel_keys == null)
    {
        global.postMessage({ type: "video_worker__log", value: "no channel keys yet, frame dropped" });
        return;
    }

    let bytes = new Uint8Array(data.data);
    let piece_size = VIDEO_PACKET_MAX_BYTES - VIDEO_WORKER_HEADER_BYTES;
    let packet_count = Math.max(1, Math.ceil(bytes.length / piece_size));
    let packets = [];
    let timestamp_low = Math.floor(data.timestamp) >>> 0;

    if (packet_count > VIDEO_WORKER_MAX_PACKETS_PER_FRAME)
    {
        global.postMessage({ type: "video_worker__log", value: "frame too large (" + bytes.length + " bytes), dropped" });
        return;
    }

    for (let index = 0; index < packet_count; index++)
    {
        let start = index * piece_size;
        let end = Math.min(bytes.length, start + piece_size);
        let packet = new Uint8Array(VIDEO_WORKER_HEADER_BYTES + (end - start));
        let flags = (data.is_keyframe ? 1 : 0) | ((index == packet_count - 1) ? 2 : 0);

        packet[0] = data.epoch & 0xff;
        packet[1] = data.frame_number & 0xff;
        packet[2] = (data.frame_number >> 8) & 0xff;
        packet[3] = index & 0xff;
        packet[4] = (index >> 8) & 0xff;
        packet[5] = packet_count & 0xff;
        packet[6] = (packet_count >> 8) & 0xff;
        packet[7] = flags;
        packet[8] = timestamp_low & 0xff;
        packet[9] = (timestamp_low >> 8) & 0xff;
        packet[10] = (timestamp_low >> 16) & 0xff;
        packet[11] = (timestamp_low >> 24) & 0xff;
        packet.set(bytes.subarray(start, end), VIDEO_WORKER_HEADER_BYTES);

        let encrypted = keys__encrypt_data_with_aes_keys(video_worker_channel_keys, packet);
        let buffer = (encrypted.byteOffset == 0 && encrypted.byteLength == encrypted.buffer.byteLength) ? encrypted.buffer : encrypted.slice().buffer;

        packets.push(buffer);
    }

    global.postMessage({ type: "video_worker__packets_ready", epoch: data.epoch, packets: packets }, packets);
}

/**
 * @brief creates the decoder for the announced codec; annex b h.264 and vp8 need no out-of-band description
 *
 * @param string codec -> the webcodecs codec string
 * @param number width -> coded width
 * @param number height -> coded height
 *
 * @return void
 */
function video_worker__decoder_setup(codec, width, height)
{
    video_worker__decoder_close();

    if (typeof VideoDecoder !== "function")
    {
        global.postMessage({ type: "video_worker__decoder_error", value: "no webcodecs in the worker" });
        return;
    }

    try
    {
        video_worker_decoder = new VideoDecoder({
            output: video_worker__on_decoded_frame,
            error: function(error)
            {
                global.postMessage({ type: "video_worker__decoder_error", value: error.message });
                video_worker_waiting_for_keyframe = true;
                // a broken decoder is rebuilt on the spot so the next keyframe can be decoded
                video_worker__decoder_setup(video_worker_decoder_codec, width, height);
            }
        });

        video_worker_decoder.configure({ codec: codec, codedWidth: width, codedHeight: height, optimizeForLatency: true });
        video_worker_decoder_codec = codec;
        video_worker_waiting_for_keyframe = true;
        video_worker_assembly = null;
        video_worker_has_drawn = false;
        video_worker_epoch = -1;
    }
    catch (setup_error)
    {
        video_worker_decoder = null;
        global.postMessage({ type: "video_worker__decoder_error", value: setup_error.message });
    }
}

/**
 * @brief closes the decoder and forgets the frame in progress
 *
 * @return void
 */
function video_worker__decoder_close()
{
    if (video_worker_decoder != null)
    {
        try { video_worker_decoder.close(); } catch (close_error) { }
        video_worker_decoder = null;
    }

    video_worker_assembly = null;
    video_worker_waiting_for_keyframe = true;
    video_worker_has_drawn = false;
}

/**
 * @brief paints the canvas black between streams
 *
 * @return void
 */
function video_worker__clear_canvas()
{
    if (video_worker_context != null && video_worker_canvas != null)
    {
        video_worker_context.fillStyle = "#000";
        video_worker_context.fillRect(0, 0, video_worker_canvas.width, video_worker_canvas.height);
    }
}

/**
 * @brief a decoded frame: drawn onto the offscreen canvas, which follows the frame's size
 *
 * @param VideoFrame frame -> the frame
 *
 * @return void
 */
function video_worker__on_decoded_frame(frame)
{
    try
    {
        if (video_worker_canvas != null && video_worker_context != null)
        {
            let width = frame.displayWidth || frame.codedWidth;
            let height = frame.displayHeight || frame.codedHeight;

            if (video_worker_canvas.width != width || video_worker_canvas.height != height)
            {
                video_worker_canvas.width = width;
                video_worker_canvas.height = height;
            }

            video_worker_context.drawImage(frame, 0, 0, width, height);

            if (video_worker_has_drawn == false)
            {
                video_worker_has_drawn = true;
                global.postMessage({ type: "video_worker__first_frame" });
            }
        }
    }
    finally
    {
        frame.close();
    }
}

/**
 * @brief asks the main thread for a keyframe, at most once a second from here (the main thread limits again)
 *
 * @return void
 */
function video_worker__ask_for_keyframe()
{
    let now = Date.now();

    if (now - video_worker_last_keyframe_request_ms < 1000)
    {
        return;
    }

    video_worker_last_keyframe_request_ms = now;
    global.postMessage({ type: "video_worker__keyframe_needed" });
}

/**
 * @brief one packet from the datachannel (sender id already stripped): decrypted, checked, put into the frame being reassembled; a complete frame goes to the decoder
 *        gaps of any kind (a lost packet, a new frame before the old one completed, a decrypt failure
 *        showing as a nonsense header) drop the frame and wait for the next keyframe
 *
 * @param ArrayBuffer buffer -> the encrypted packet
 *
 * @return void
 */
function video_worker__decode_packet(buffer)
{
    if (video_worker_channel_keys == null || video_worker_decoder == null)
    {
        return;
    }

    let packet = keys__decrypt_data_with_aes_keys(video_worker_channel_keys, new Uint8Array(buffer));

    if (packet == null || packet.length <= VIDEO_WORKER_HEADER_BYTES)
    {
        video_worker__mark_gap();
        return;
    }

    let epoch = packet[0];
    let frame_number = packet[1] | (packet[2] << 8);
    let packet_index = packet[3] | (packet[4] << 8);
    let packet_count = packet[5] | (packet[6] << 8);
    let flags = packet[7];
    let timestamp = (packet[8] | (packet[9] << 8) | (packet[10] << 16) | (packet[11] << 24)) >>> 0;
    let is_keyframe = (flags & 1) != 0;

    // a header that cannot be right is a decrypt with the wrong keys (a rotation in flight) or damage
    if (packet_count == 0 || packet_count > VIDEO_WORKER_MAX_PACKETS_PER_FRAME || packet_index >= packet_count)
    {
        video_worker__mark_gap();
        return;
    }

    if (epoch != video_worker_epoch)
    {
        // a new stream (or the first packet): start clean and wait for its keyframe
        video_worker_epoch = epoch;
        video_worker_assembly = null;
        video_worker_waiting_for_keyframe = true;
    }

    if (video_worker_assembly == null || video_worker_assembly.frame_number != frame_number)
    {
        if (video_worker_assembly != null && video_worker_assembly.received < video_worker_assembly.packet_count)
        {
            // the previous frame never completed: the reference chain is broken until a keyframe
            video_worker__mark_gap();
        }

        video_worker_assembly = {
            frame_number: frame_number,
            packet_count: packet_count,
            received: 0,
            parts: new Array(packet_count),
            is_keyframe: is_keyframe,
            timestamp: timestamp,
            total_bytes: 0
        };
    }

    if (video_worker_assembly.parts[packet_index] != null)
    {
        return; // a duplicate
    }

    video_worker_assembly.parts[packet_index] = packet.subarray(VIDEO_WORKER_HEADER_BYTES);
    video_worker_assembly.received++;
    video_worker_assembly.total_bytes += packet.length - VIDEO_WORKER_HEADER_BYTES;

    if (video_worker_assembly.received < video_worker_assembly.packet_count)
    {
        return;
    }

    // the frame is complete
    let assembly = video_worker_assembly;
    video_worker_assembly = null;

    if (video_worker_waiting_for_keyframe == true && assembly.is_keyframe == false)
    {
        video_worker__ask_for_keyframe();
        return;
    }

    let frame_bytes = new Uint8Array(assembly.total_bytes);
    let offset = 0;

    for (let i = 0; i < assembly.packet_count; i++)
    {
        frame_bytes.set(assembly.parts[i], offset);
        offset += assembly.parts[i].length;
    }

    try
    {
        if (video_worker_decoder.state != "configured")
        {
            return;
        }

        video_worker_decoder.decode(new EncodedVideoChunk({
            type: assembly.is_keyframe ? "key" : "delta",
            timestamp: assembly.timestamp,
            data: frame_bytes
        }));

        video_worker_waiting_for_keyframe = false;
    }
    catch (decode_error)
    {
        global.postMessage({ type: "video_worker__decoder_error", value: decode_error.message });
        video_worker_waiting_for_keyframe = true;
        video_worker__ask_for_keyframe();
    }
}

/**
 * @brief something was lost: only a keyframe can restart decoding, so one is requested
 *
 * @return void
 */
function video_worker__mark_gap()
{
    video_worker_assembly = null;
    video_worker_waiting_for_keyframe = true;
    video_worker__ask_for_keyframe();
}
