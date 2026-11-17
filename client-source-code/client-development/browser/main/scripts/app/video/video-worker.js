// runs packet encryption, video decoding and OffscreenCanvas drawing in the video worker
// the page forwards encoded frames and received packet buffers
// channel encryption protects the media body. server and session layers authenticate transport
// web Crypto and the JavaScript fallback produce compatible AES and HMAC results
// the plaintext packet header is 12 bytes, with little endian integers:
// [1 B epoch][2 B frame number][2 B packet index][2 B packet count][1 B flags][4 B timestamp]
// the timestamp stores the low 32 bits of the encoded chunk time in microseconds
// flags: bit 0 keyframe, bit 1 final packet, bit 2 sound packet
// sound carries one complete Opus packet with its own sequence number
// the page sends it to the Opus worker. picture delay matches the sound playback buffer

var VIDEO_WORKER_HEADER_BYTES = 12;
var VIDEO_WORKER_MAX_PACKETS_PER_FRAME = 256;
var VIDEO_WORKER_FLAG_KEYFRAME = 1;
var VIDEO_WORKER_FLAG_LAST_PACKET = 2;
var VIDEO_WORKER_FLAG_SOUND = 4;
var VIDEO_WORKER_MAX_HELD_PICTURES = 16;
var VIDEO_WORKER_FRAME_DEADLINE_MS = 250;
var VIDEO_WORKER_KEYFRAME_DEADLINE_MS = 1500;
var VIDEO_WORKER_MAX_DECODER_FRAMES = 32;
var VIDEO_WORKER_MAX_PENDING_PACKETS = 256;

var video_worker_channel_keys = null;      // the channel's key layers as the main thread holds them (key_bytes, iv_bytes)
var video_worker_native_keys = null;       // imported channel keys for Web Crypto. null selects aes-js
var video_worker_transport = null;         // the keys of the second encryption: { session: { key_bytes, mac_bytes }, server_layers: [{ key_bytes }], local_client_id } (keys.js)
var video_worker_native_transport = null;  // imported server, session and MAC keys. null selects the JavaScript fallback
var video_worker_streamer_client_id = -1;  // the viewer side decodes packets of this sender only
var video_worker_frame_counter = null;     // outgoing packet counter used by keys__counter_create
var video_worker_keys_generation = 0;      // counts channel key sets, so a late import of a replaced set is discarded
var video_worker_transport_generation = 0; // the same for the transport keys, which arrive independently of the channel keys
var video_worker_send_chain = Promise.resolve();     // frames are encrypted and posted in encoder order
var video_worker_receive_chain = Promise.resolve();  // packets are decrypted and reassembled in arrival order
var video_worker_canvas = null;
var video_worker_context = null;
var video_worker_decoder = null;
var video_worker_decoder_codec = "";
var video_worker_decoder_config = null;
var video_worker_waiting_for_keyframe = true;
var video_worker_assembly = null;          // the frame being reassembled: { epoch, frame_number, packet_count, received, parts, is_keyframe, timestamp }
var video_worker_epoch = -1;
var video_worker_has_drawn = false;
var video_worker_last_keyframe_request_ms = 0;
var video_worker_picture_hold_ms = 0;      // while the stream has sound: how long a decoded picture waits before it is drawn
var video_worker_held_pictures = [];       // the pictures waiting: { bitmap, width, height, is_failed, due_ms } in decode order
var video_worker_receive_generation = 0;
var video_worker_pending_packets = 0;
var video_worker_last_frame_number = null;
var video_worker_clock_offset = null;
var video_worker_last_timestamp = null;
var video_worker_timestamp_ms = 0;
var video_worker_decode_deadlines = new Map();

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
        video_worker__set_channel_keys(data.value);
    }
    else if (data.type == "mainthread__transport_keys_for_video_worker")
    {
        video_worker__set_transport_keys(data);
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
        video_worker__decoder_setup(data.codec, data.width, data.height, data.streamer_client_id, data.picture_hold_ms);
    }
    else if (data.type == "mainthread__video_worker_decoder_reset")
    {
        video_worker__decoder_close();
        video_worker__clear_canvas();
    }
    else if (data.type == "mainthread__video_worker_decode_packet")
    {
        video_worker__decode_packet(data.value, data.received_at);
    }
}

/**
 * @brief replaces channel keys and discards imports belonging to an older key set
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes. null clears them
 *
 * @return void
 */
function video_worker__set_channel_keys(keys)
{
    let generation = ++video_worker_keys_generation;
    let imports = [];

    video_worker_channel_keys = keys;
    video_worker_native_keys = null;

    if (keys == null || typeof crypto === "undefined" || crypto.subtle == null)
    {
        return;
    }

    for (let i = 0; i < keys.length; i++)
    {
        imports.push(crypto.subtle.importKey("raw", new Uint8Array(keys[i].key_bytes), { name: "AES-CTR" }, false, ["encrypt", "decrypt"]));
    }

    Promise.all(imports).then(function(imported)
    {
        if (generation == video_worker_keys_generation)
        {
            video_worker_native_keys = imported;
        }
    }, function(error)
    {
        global.postMessage({ type: "video_worker__log", value: "native aes unavailable, aes-js in use: " + error.message });
    });
}

/**
 * @brief installs the current server and session keys for video transport
 *
 * @param object data -> session { key_bytes, mac_bytes } or null, server_layers [{ key_bytes }], local_client_id
 *
 * @return void
 */
function video_worker__set_transport_keys(data)
{
    let generation = ++video_worker_transport_generation;
    let imports = [];
    video_worker_transport = { session: data.session, server_layers: data.server_layers || [], local_client_id: data.local_client_id };
    video_worker_native_transport = null;
    if (data.session == null || data.session.layers != null || video_worker_transport.server_layers.some(function(layer)
    {
        return layer.key_type != null && layer.key_type !== "aes256";
    }) || typeof crypto === "undefined" || crypto.subtle == null)
    {
        return;
    }
    for (let i = 0; i < video_worker_transport.server_layers.length; i++)
    {
        imports.push(crypto.subtle.importKey("raw", new Uint8Array(video_worker_transport.server_layers[i].key_bytes), { name: "AES-CTR" }, false, ["encrypt", "decrypt"]));
    }
    imports.push(crypto.subtle.importKey("raw", new Uint8Array(data.session.key_bytes), { name: "AES-CTR" }, false, ["encrypt", "decrypt"]));
    imports.push(crypto.subtle.importKey("raw", new Uint8Array(data.session.mac_bytes), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]));
    Promise.all(imports).then(function(imported)
    {
        if (generation == video_worker_transport_generation)
        {
            video_worker_native_transport = { server: imported.slice(0, imported.length - 2), session: imported[imported.length - 2], mac: imported[imported.length - 1] };
        }
    }, function(error)
    {
        global.postMessage({ type: "video_worker__log", value: "native crypto unavailable for the transport keys, javascript in use: " + error.message });
    });
}

/**
 * @brief applies one AES counter layer through Web Crypto or aes-js
 * encryption and decryption use the same counter operation
 *
 * @param array key_bytes -> the 32 byte key
 * @param CryptoKey native_key -> the imported key, or null for aes-js
 * @param Uint8Array block -> the counter block
 * @param Uint8Array bytes -> the data
 *
 * @return Promise resolves with the Uint8Array result
 */
function video_worker__layer(key_bytes, native_key, block, bytes)
{
    if (native_key == null)
    {
        return Promise.resolve(keys__aes_ctr(key_bytes, block, bytes));
    }
    return crypto.subtle.encrypt({ name: "AES-CTR", counter: block, length: 128 }, native_key, bytes).then(function(result)
    {
        return new Uint8Array(result);
    });
}

/**
 * @brief the inner layers (the channel keys or the server keys) over some bytes, one after the other
 *
 * @param array layers -> [{ key_bytes, iv_bytes }], iv_bytes may be null
 * @param array native_keys -> the imported keys in the same order, or null for aes-js
 * @param number sender_id -> the sending client's id, part of every counter block
 * @param Uint8Array counter_bytes -> the sender counter, part of every counter block
 * @param Uint8Array bytes -> the data
 * @param boolean is_encrypt -> true applies the layers first to last, false last to first
 *
 * @return Promise resolves with the Uint8Array result
 */
function video_worker__inner_layers(layers, native_keys, sender_id, counter_bytes, bytes, is_encrypt)
{
    let chain = Promise.resolve(bytes);
    for (let step = 0; step < layers.length; step++)
    {
        let i = is_encrypt ? step : (layers.length - 1 - step);
        chain = chain.then(function(current)
        {
            let block = keys__media_inner_block(sender_id, counter_bytes, layers[i].iv_bytes);
            if (layers[i].key_type != null && layers[i].key_type !== "aes256")
            {
                return connection_crypto__crypt(layers[i].key_type, layers[i].key_bytes, block, current);
            }
            return video_worker__layer(layers[i].key_bytes, native_keys != null ? native_keys[i] : null, block, current);
        });
    }
    return chain;
}

/**
 * @brief calculates the session authentication code through Web Crypto or the JavaScript fallback
 *
 * @param Uint8Array bytes -> the data
 * @param boolean receiving -> selects the receive key in the JavaScript fallback
 *
 * @return Promise resolves with the 32 byte code as a Uint8Array
 */
function video_worker__authentication_code(bytes, receiving = false)
{
    let native = video_worker_native_transport;
    if (native == null)
    {
        return Promise.resolve(new Uint8Array(keys___hmac_sha256(receiving && video_worker_transport.session.receive_mac_bytes ? video_worker_transport.session.receive_mac_bytes : video_worker_transport.session.mac_bytes, bytes)));
    }
    return crypto.subtle.sign("HMAC", native.mac, bytes).then(function(result)
    {
        return new Uint8Array(result);
    });
}

/**
 * @brief applies channel and transport encryption with a fresh packet counter
 *
 * @param Uint8Array packet -> the packet with its 12 byte header
 *
 * @return Promise resolves with the Uint8Array frame
 */
function video_worker__encrypt_packet(packet)
{
    let transport = video_worker_transport;
    let native = video_worker_native_transport;
    let sender_id = transport.local_client_id;
    if (video_worker_frame_counter == null)
    {
        video_worker_frame_counter = keys__counter_create();
    }
    keys__counter_increment(video_worker_frame_counter);
    let counter = video_worker_frame_counter.slice();
    return video_worker__inner_layers(video_worker_channel_keys, video_worker_native_keys, sender_id, counter, packet, true).then(function(inner)
    {
        return video_worker__inner_layers(transport.server_layers, native != null ? native.server : null, sender_id, counter, inner, true);
    }).then(function(inner)
    {
        let body = keys__media_build_body(sender_id, counter, inner);
        if (transport.session.layers != null)
        {
            return keys__media_session_crypt(transport.session, keys__media_session_block(counter, MEDIA_DIRECTION_TO_SERVER, MEDIA_STREAM_VIDEO), body, true);
        }
        return video_worker__layer(transport.session.key_bytes, native != null ? native.session : null, keys__media_session_block(counter, MEDIA_DIRECTION_TO_SERVER, MEDIA_STREAM_VIDEO), body);
    }).then(function(encrypted_body)
    {
        let frame = new Uint8Array(MEDIA_FRAME_HEADER + encrypted_body.length + MEDIA_FRAME_CODE);
        frame[0] = MEDIA_FRAME_VERSION;
        frame.set(counter, 1);
        frame.set(encrypted_body, MEDIA_FRAME_HEADER);
        return video_worker__authentication_code(frame.subarray(0, MEDIA_FRAME_HEADER + encrypted_body.length)).then(function(code)
        {
            frame.set(code.subarray(0, MEDIA_FRAME_CODE), MEDIA_FRAME_HEADER + encrypted_body.length);
            return frame;
        });
    });
}

/**
 * @brief authenticates a frame and removes its encryption layers in reverse order
 * reject frames from another streamer or with an invalid authentication code
 *
 * @param Uint8Array frame -> the frame
 *
 * @return Promise resolves with the Uint8Array packet, or null
 */
function video_worker__decrypt_packet(frame)
{
    let transport = video_worker_transport;
    let native = video_worker_native_transport;
    if (frame.length <= MEDIA_FRAME_OVERHEAD || frame[0] !== MEDIA_FRAME_VERSION)
    {
        return Promise.resolve(null);
    }
    let code_offset = frame.length - MEDIA_FRAME_CODE;
    return video_worker__authentication_code(frame.subarray(0, code_offset), true).then(function(expected)
    {
        let difference = 0;
        for (let i = 0; i < MEDIA_FRAME_CODE; i++)
        {
            difference |= expected[i] ^ frame[code_offset + i];
        }
        if (difference !== 0)
        {
            return null;
        }
        let opened = transport.session.layers != null ? Promise.resolve(keys__media_session_crypt(transport.session, keys__media_session_block(frame.subarray(1, MEDIA_FRAME_HEADER), MEDIA_DIRECTION_FROM_SERVER, MEDIA_STREAM_VIDEO), frame.subarray(MEDIA_FRAME_HEADER, code_offset), false)) : video_worker__layer(transport.session.key_bytes, native != null ? native.session : null, keys__media_session_block(frame.subarray(1, MEDIA_FRAME_HEADER), MEDIA_DIRECTION_FROM_SERVER, MEDIA_STREAM_VIDEO), frame.subarray(MEDIA_FRAME_HEADER, code_offset));
        return opened.then(function(body)
        {
            let sender_id = (body[0] | (body[1] << 8) | (body[2] << 16) | (body[3] << 24)) >>> 0;
            if (sender_id !== video_worker_streamer_client_id)
            {
                return null;
            }
            let counter = body.slice(4, MEDIA_BODY_HEADER);
            return video_worker__inner_layers(transport.server_layers, native != null ? native.server : null, sender_id, counter, body.subarray(MEDIA_BODY_HEADER), false).then(function(inner)
            {
                return video_worker__inner_layers(video_worker_channel_keys, video_worker_native_keys, sender_id, counter, inner, false);
            });
        });
    });
}

/**
 * @brief splits an encoded frame into encrypted packets and posts them for sending
 *
 * @param object data -> epoch, frame_number, is_keyframe, timestamp and the frame bytes (ArrayBuffer)
 *
 * @return void
 */
function video_worker__packetize(data)
{
    if (video_worker_channel_keys == null || video_worker_transport == null || video_worker_transport.session == null)
    {
        video_worker__report_dropped_frame(data);
        return;
    }

    let bytes = new Uint8Array(data.data);
    let piece_size = VIDEO_PACKET_MAX_BYTES - VIDEO_WORKER_HEADER_BYTES;
    let packet_count = Math.max(1, Math.ceil(bytes.length / piece_size));
    let packets = [];
    let timestamp_low = Math.floor(data.timestamp) >>> 0;
    let epoch = data.epoch;
    let is_sound = (data.is_sound == true);

    if (packet_count > VIDEO_WORKER_MAX_PACKETS_PER_FRAME)
    {
        video_worker__report_dropped_frame(data);
        return;
    }

    // sound fits in one Opus packet and is not split
    if (is_sound == true && packet_count != 1)
    {
        video_worker__report_dropped_frame(data);
        return;
    }

    for (let index = 0; index < packet_count; index++)
    {
        let start = index * piece_size;
        let end = Math.min(bytes.length, start + piece_size);
        let packet = new Uint8Array(VIDEO_WORKER_HEADER_BYTES + (end - start));
        let flags = is_sound ? VIDEO_WORKER_FLAG_SOUND : ((data.is_keyframe ? VIDEO_WORKER_FLAG_KEYFRAME : 0) | ((index == packet_count - 1) ? VIDEO_WORKER_FLAG_LAST_PACKET : 0));

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
        packets.push(packet);
    }

    // chain asynchronous encryption to preserve encoder order
    video_worker_send_chain = video_worker_send_chain.then(function()
    {
        if (performance.timeOrigin + performance.now() > data.expires_at)
        {
            return null;
        }
        return Promise.all(packets.map(function(packet)
        {
            return video_worker__encrypt_packet(packet);
        }));
    }).then(function(encrypted_packets)
    {
        if (encrypted_packets === null || performance.timeOrigin + performance.now() > data.expires_at)
        {
            video_worker__report_dropped_frame(data);
            return;
        }
        let buffers = encrypted_packets.map(function(encrypted)
        {
            return (encrypted.byteOffset == 0 && encrypted.byteLength == encrypted.buffer.byteLength) ? encrypted.buffer : encrypted.slice().buffer;
        });

        global.postMessage({ type: "video_worker__packets_ready", epoch: epoch,
            generation: data.generation, frame_number: data.frame_number,
            expires_at: data.expires_at, is_sound: is_sound, is_keyframe: data.is_keyframe === true, packets: buffers }, buffers);
    }, function(error)
    {
        video_worker__report_dropped_frame(data);
    });
}

/**
 * @brief lets the sender continue after a frame was dropped during encryption
 *
 * @param object data -> the frame's stream, deadline and sound flag
 *
 * @return void
 */
function video_worker__report_dropped_frame(data)
{
    global.postMessage({ type: "video_worker__frame_dropped", epoch: data.epoch,
        generation: data.generation, frame_number: data.frame_number,
        expires_at: data.expires_at, is_sound: data.is_sound === true });
}

/**
 * @brief creates the announced WebCodecs decoder
 * annex B H.264 and VP8 do not need a separate codec description
 *
 * @param string codec -> the webcodecs codec string
 * @param number width -> coded width
 * @param number height -> coded height
 * @param number streamer_client_id -> the client whose packets are decoded. packets of anybody else are dropped
 * @param number picture_hold_ms -> delay before drawing in milliseconds, or zero to draw immediately
 *
 * @return void
 */
function video_worker__decoder_setup(codec, width, height, streamer_client_id, picture_hold_ms)
{
    video_worker__decoder_close();
    video_worker_streamer_client_id = (typeof streamer_client_id === "number") ? streamer_client_id : -1;
    video_worker_picture_hold_ms = (typeof picture_hold_ms === "number" && picture_hold_ms > 0) ? picture_hold_ms : 0;

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
                // rebuild a failed decoder while keeping the streamer and picture delay
                video_worker__decoder_setup(video_worker_decoder_codec, width, height, video_worker_streamer_client_id, video_worker_picture_hold_ms);
            }
        });

        video_worker_decoder_config = { codec: codec, codedWidth: width, codedHeight: height, optimizeForLatency: true };
        video_worker_decoder.configure(video_worker_decoder_config);
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
    video_worker_receive_generation++;
    video_worker_pending_packets = 0;
    video_worker_receive_chain = Promise.resolve();
    video_worker_last_frame_number = null;
    video_worker_clock_offset = null;
    video_worker_last_timestamp = null;
    video_worker_decode_deadlines.clear();
    video_worker_decoder_config = null;
    if (video_worker_decoder != null)
    {
        try
        {
            video_worker_decoder.close();
        }
        catch (close_error)
        {
        }
        video_worker_decoder = null;
    }

    video_worker_assembly = null;
    video_worker_waiting_for_keyframe = true;
    video_worker_has_drawn = false;
    video_worker__drop_held_pictures();
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
 * @brief draws a decoded frame immediately or copies it for delayed playback
 * release the decoder frame after copying so held pictures cannot stall decoding
 *
 * @param VideoFrame frame -> the frame
 *
 * @return void
 */
function video_worker__on_decoded_frame(frame)
{
    let width = frame.displayWidth || frame.codedWidth;
    let height = frame.displayHeight || frame.codedHeight;
    let due_ms = video_worker_decode_deadlines.get(frame.timestamp);
    let now = performance.timeOrigin + performance.now();

    video_worker_decode_deadlines.delete(frame.timestamp);
    if (due_ms === undefined || now > due_ms + VIDEO_WORKER_FRAME_DEADLINE_MS)
    {
        frame.close();
        return;
    }

    if (video_worker_picture_hold_ms <= 0 || typeof createImageBitmap !== "function")
    {
        video_worker__draw_picture(frame, width, height);
        frame.close();
        return;
    }

    let held = { bitmap: null, width: width, height: height, is_failed: false, due_ms: due_ms };

    video_worker_held_pictures.push(held);

    createImageBitmap(frame).then(function(bitmap)
    {
        // an entry dropped or skipped meanwhile is marked failed: its late copy is closed, not kept
        if (held.is_failed == true)
        {
            bitmap.close();
        }
        else
        {
            held.bitmap = bitmap;
        }
        frame.close();
    }, function()
    {
        if (!held.is_failed && performance.timeOrigin + performance.now() <= held.due_ms + VIDEO_WORKER_FRAME_DEADLINE_MS)
        {
            video_worker__draw_picture(frame, width, height);
        }
        held.is_failed = true;
        frame.close();
    });

    // release old pictures when drawing or bitmap copying falls behind
    while (video_worker_held_pictures.length > VIDEO_WORKER_MAX_HELD_PICTURES)
    {
        video_worker__draw_oldest_held_picture(true);
    }

    setTimeout(video_worker__draw_due_held_pictures, Math.max(0, due_ms - now));
}

/**
 * @brief draws the newest picture that is due and releases older pictures that were delayed
 *
 * @return void
 */
function video_worker__draw_due_held_pictures()
{
    let now = performance.timeOrigin + performance.now();

    while (video_worker_held_pictures.length > 1 && video_worker_held_pictures[1].due_ms <= now)
    {
        video_worker__draw_oldest_held_picture(true);
    }
    while (video_worker_held_pictures.length > 0 && video_worker_held_pictures[0].due_ms <= now)
    {
        if (video_worker__draw_oldest_held_picture(false) == false)
        {
            // the copy of the oldest picture is still in progress: try again shortly, in order
            setTimeout(video_worker__draw_due_held_pictures, 5);
            return;
        }
    }
}

/**
 * @brief draws the oldest held picture, or skips it when its copy failed
 *
 * @param boolean is_forced -> true to discard this picture without drawing it
 *
 * @return boolean false when the oldest picture is not ready and was not forced
 */
function video_worker__draw_oldest_held_picture(is_forced)
{
    let held = video_worker_held_pictures[0];

    if (held == null)
    {
        return true;
    }

    if (is_forced || performance.timeOrigin + performance.now() > held.due_ms + VIDEO_WORKER_FRAME_DEADLINE_MS)
    {
        held.is_failed = true;
    }

    if (held.bitmap == null && held.is_failed == false)
    {
        if (is_forced == false)
        {
            return false;
        }
        held.is_failed = true; // the late copy is closed on arrival
    }

    video_worker_held_pictures.shift();

    if (held.bitmap != null)
    {
        if (!held.is_failed)
        {
            video_worker__draw_picture(held.bitmap, held.width, held.height);
        }
        held.bitmap.close();
    }

    return true;
}

/**
 * @brief closes every held picture without drawing it (a decoder reset or close)
 *
 * @return void
 */
function video_worker__drop_held_pictures()
{
    for (let i = 0; i < video_worker_held_pictures.length; i++)
    {
        if (video_worker_held_pictures[i].bitmap != null)
        {
            video_worker_held_pictures[i].bitmap.close();
        }
        video_worker_held_pictures[i].is_failed = true;
    }

    video_worker_held_pictures = [];
}

/**
 * @brief draws one picture onto the offscreen canvas, which follows the picture's size
 *
 * @param VideoFrame|ImageBitmap picture -> the picture
 * @param number width -> its width
 * @param number height -> its height
 *
 * @return void
 */
function video_worker__draw_picture(picture, width, height)
{
    if (video_worker_canvas == null || video_worker_context == null)
    {
        return;
    }

    if (video_worker_canvas.width != width || video_worker_canvas.height != height)
    {
        video_worker_canvas.width = width;
        video_worker_canvas.height = height;
    }

    video_worker_context.drawImage(picture, 0, 0, width, height);

    if (video_worker_has_drawn == false)
    {
        video_worker_has_drawn = true;
        global.postMessage({ type: "video_worker__first_frame" });
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
 * @brief decrypts received video packets in order and reports failures as gaps
 *
 * @param ArrayBuffer buffer -> the encrypted packet
 * @param number received_at -> arrival time, including the page's performance.timeOrigin
 *
 * @return void
 */
function video_worker__decode_packet(buffer, received_at)
{
    let generation = video_worker_receive_generation;

    if (video_worker_channel_keys == null || video_worker_transport == null || video_worker_transport.session == null || video_worker_decoder == null)
    {
        return;
    }

    if (video_worker_pending_packets >= VIDEO_WORKER_MAX_PENDING_PACKETS)
    {
        video_worker__mark_gap();
        return;
    }
    video_worker_pending_packets++;

    video_worker_receive_chain = video_worker_receive_chain.then(function()
    {
        if (generation !== video_worker_receive_generation || performance.timeOrigin + performance.now() - received_at > VIDEO_WORKER_FRAME_DEADLINE_MS)
        {
            return null;
        }
        return video_worker__decrypt_packet(new Uint8Array(buffer));
    }).then(function(packet)
    {
        if (generation !== video_worker_receive_generation)
        {
            return;
        }
        if (packet == null || performance.timeOrigin + performance.now() - received_at > VIDEO_WORKER_FRAME_DEADLINE_MS)
        {
            video_worker__mark_gap();
            return;
        }
        video_worker__process_packet(packet, received_at);
    }, function(error)
    {
        if (generation === video_worker_receive_generation)
        {
            video_worker__mark_gap();
        }
    }).then(function()
    {
        if (generation === video_worker_receive_generation)
        {
            video_worker_pending_packets--;
        }
    });
}

/**
 * @brief reassembles validated packets and sends complete frames to the decoder
 *
 * @param Uint8Array packet -> the decrypted packet, header first
 * @param number received_at -> packet arrival time in milliseconds
 *
 * @return void
 */
function video_worker__process_packet(packet, received_at)
{
    let arrival_time = 0;
    let assembly_limit = 0;

    // the decoder may have been closed while the packet was being decrypted
    if (video_worker_decoder == null)
    {
        return;
    }

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
    let is_keyframe = (flags & VIDEO_WORKER_FLAG_KEYFRAME) != 0;

    // a header that cannot be right is a decrypt with the wrong keys (a key change still in progress) or damage
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
        video_worker_last_frame_number = null;
        video_worker_clock_offset = null;
        video_worker_last_timestamp = null;
    }

    if ((flags & VIDEO_WORKER_FLAG_SOUND) != 0)
    {
        // a sound packet is never part of a picture: its opus packet goes to the main thread, which feeds the opus decoder worker
        let opus_bytes = packet.slice(VIDEO_WORKER_HEADER_BYTES);

        global.postMessage({ type: "video_worker__sound_packet", sequence_number: frame_number, timestamp: timestamp, value: opus_bytes.buffer }, [opus_bytes.buffer]);
        return;
    }

    if (video_worker_assembly == null || video_worker_assembly.frame_number != frame_number)
    {
        if (video_worker_last_frame_number !== null)
        {
            let distance = (frame_number - video_worker_last_frame_number) & 0xffff;

            if (distance === 0 || distance > 0x8000)
            {
                return;
            }
            if (distance !== 1)
            {
                video_worker__mark_gap();
            }
        }
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
            total_bytes: 0,
            received_at: received_at
        };
    }

    if (video_worker_assembly.packet_count !== packet_count || video_worker_assembly.timestamp !== timestamp || video_worker_assembly.is_keyframe !== is_keyframe)
    {
        video_worker__mark_gap();
        return;
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
    video_worker_last_frame_number = frame_number;

    // compare elapsed sender time with arrival time without requiring synchronized clocks
    if (video_worker_last_timestamp === null)
    {
        video_worker_timestamp_ms = 0;
    }
    else
    {
        video_worker_timestamp_ms += ((timestamp - video_worker_last_timestamp) | 0) / 1000;
    }
    video_worker_last_timestamp = timestamp;
    // a full picture can take longer to arrive than the smaller pictures that follow it
    arrival_time = is_keyframe ? received_at : assembly.received_at;
    assembly_limit = is_keyframe ? VIDEO_WORKER_KEYFRAME_DEADLINE_MS : VIDEO_WORKER_FRAME_DEADLINE_MS;

    video_worker_clock_offset = Math.min(video_worker_clock_offset === null || is_keyframe ? Infinity : video_worker_clock_offset, arrival_time - video_worker_timestamp_ms);
    let due_ms = video_worker_clock_offset + video_worker_timestamp_ms;
    let now = performance.timeOrigin + performance.now();

    // some decoders need several inputs before returning a picture. old deadlines must not stop new input
    for (let [frame_timestamp, deadline] of video_worker_decode_deadlines)
    {
        if (now > deadline + VIDEO_WORKER_FRAME_DEADLINE_MS)
        {
            video_worker_decode_deadlines.delete(frame_timestamp);
        }
    }

    if (now - assembly.received_at > assembly_limit || now - received_at > VIDEO_WORKER_FRAME_DEADLINE_MS || now - due_ms > VIDEO_WORKER_FRAME_DEADLINE_MS || (!video_worker_waiting_for_keyframe && (video_worker_decoder.decodeQueueSize >= 3 || video_worker_decode_deadlines.size >= VIDEO_WORKER_MAX_DECODER_FRAMES)))
    {
        video_worker__mark_gap();
        return;
    }

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
        if (video_worker_waiting_for_keyframe && assembly.is_keyframe)
        {
            // discard unfinished decoding before starting again from a complete picture
            video_worker_decoder.reset();
            video_worker_decoder.configure(video_worker_decoder_config);
            video_worker_decode_deadlines.clear();
        }
        if (video_worker_decoder.state != "configured")
        {
            return;
        }

        video_worker_decode_deadlines.set(assembly.timestamp, due_ms + video_worker_picture_hold_ms);
        video_worker_decoder.decode(new EncodedVideoChunk({
            type: assembly.is_keyframe ? "key" : "delta",
            timestamp: assembly.timestamp,
            data: frame_bytes
        }));

        video_worker_waiting_for_keyframe = false;
    }
    catch (decode_error)
    {
        video_worker_decode_deadlines.delete(assembly.timestamp);
        global.postMessage({ type: "video_worker__decoder_error", value: decode_error.message });
        video_worker_waiting_for_keyframe = true;
        video_worker__ask_for_keyframe();
    }
}

/**
 * @brief requests a keyframe after packet loss or decoding failure
 *
 * @return void
 */
function video_worker__mark_gap()
{
    video_worker_assembly = null;
    video_worker_waiting_for_keyframe = true;
    video_worker__ask_for_keyframe();
}
