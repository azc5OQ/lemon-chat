// video-worker.js is embedded in template.html along with the other client files
// it runs inside the "video_worker" web worker only (see worker-entry.js). streamer side: it splits an
// encoded frame into packets and encrypts each with the channel keys. viewer side: it decrypts and
// reassembles packets into frames, decodes them with webcodecs and draws them on the popup's canvas,
// which the main thread handed over as an OffscreenCanvas. the main thread only forwards buffers
//
// every packet is encrypted in layers, the same way as a voice frame: first with the channel keys, which
// only the members of the channel have, then with the server keys, and then a second time with this
// client's session keys plus an authentication code, so the server can check who sent a packet and nobody
// on the network path can read or forge one (keys.js, the block "the encryption of audio and video
// frames", explains the layers and the frame layout; media_auth.c is the server side). the encryption
// runs on the browser's own AES and HMAC (crypto.subtle) when they are available and falls back to the
// pure javascript aes-js and sha256 otherwise; both produce the same bytes, so a viewer on one path
// decrypts a streamer on the other
//
// packet layout inside the encryption (12 byte header, little endian):
//   [1 B stream epoch][2 B frame number][2 B packet index][2 B packet count][1 B flags][4 B timestamp, low 32 bits of the chunk's microseconds]
//   flags: bit0 = keyframe, bit1 = last packet of the frame

var VIDEO_WORKER_HEADER_BYTES = 12;
var VIDEO_WORKER_MAX_PACKETS_PER_FRAME = 256;

var video_worker_channel_keys = null;      // the channel's key layers as the main thread holds them (key_bytes, iv_bytes)
var video_worker_native_keys = null;       // the same keys imported into the browser's AES; null while aes-js is in use
var video_worker_transport = null;         // the keys of the second encryption: { session: { key_bytes, mac_bytes }, server_layers: [{ key_bytes }], local_client_id } (keys.js)
var video_worker_native_transport = null;  // the same keys imported into the browser's crypto: { server: [CryptoKey], session: CryptoKey, mac: CryptoKey }; null while the javascript fallback is in use
var video_worker_streamer_client_id = -1;  // the viewer side decodes packets of this sender only
var video_worker_frame_counter = null;     // counts the packets this streamer sent; part of every counter block (keys__counter_create)
var video_worker_keys_generation = 0;      // counts channel key sets, so a late import of a replaced set is discarded
var video_worker_transport_generation = 0; // the same for the transport keys, which arrive independently of the channel keys
var video_worker_send_chain = Promise.resolve();     // frames are encrypted and posted in encoder order
var video_worker_receive_chain = Promise.resolve();  // packets are decrypted and reassembled in arrival order
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
        video_worker__decoder_setup(data.codec, data.width, data.height, data.streamer_client_id);
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
 * @brief takes a new set of channel keys: aes-js can use them at once, the browser's AES after the import
 *
 *        the import is asynchronous. a set that is replaced before its import finished is discarded
 *        (generation check). without crypto.subtle, or when an import fails, aes-js stays in use.
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes; null clears them
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
 * @brief takes the keys of the second encryption: the server keys and this client's session keys
 *
 *        the main thread sends them when the stream starts or the viewer connects, and again when they
 *        change. like the channel keys they are imported into the browser's crypto when it is available
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
    if (data.session == null || typeof crypto === "undefined" || crypto.subtle == null)
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
 * @brief one AES-CTR layer over some bytes: the browser's AES when the key is imported, aes-js otherwise
 *
 *        both give the same bytes: AES-CTR with the given 16 byte counter block, all 128 bits counting. in
 *        counter mode encrypting and decrypting are the same operation
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
            return video_worker__layer(layers[i].key_bytes, native_keys != null ? native_keys[i] : null, keys__media_inner_block(sender_id, counter_bytes, layers[i].iv_bytes), current);
        });
    }
    return chain;
}

/**
 * @brief the authentication code of some bytes with the session's checksum key: the browser's HMAC when it is imported, javascript otherwise
 *
 * @param Uint8Array bytes -> the data
 *
 * @return Promise resolves with the 32 byte code as a Uint8Array
 */
function video_worker__authentication_code(bytes)
{
    let native = video_worker_native_transport;
    if (native == null)
    {
        return Promise.resolve(new Uint8Array(keys___hmac_sha256(video_worker_transport.session.mac_bytes, bytes)));
    }
    return crypto.subtle.sign("HMAC", native.mac, bytes).then(function(result)
    {
        return new Uint8Array(result);
    });
}

/**
 * @brief encrypts one packet of this streamer into a frame for the wire
 *
 *        the packet goes under the channel keys, then the server keys, gets the sender id and the counter in
 *        front, goes under the session key, and gets the authentication code at the end; the layout is in
 *        keys.js. every packet takes the next value of the frame counter
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
 * @brief opens one frame from the wire on the viewer side: the authentication code is checked, the layers come off in reverse order
 *
 *        a frame whose code does not match, or that was not sent by the channel's streamer, gives null
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
    return video_worker__authentication_code(frame.subarray(0, code_offset)).then(function(expected)
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
        return video_worker__layer(transport.session.key_bytes, native != null ? native.session : null, keys__media_session_block(frame.subarray(1, MEDIA_FRAME_HEADER), MEDIA_DIRECTION_FROM_SERVER, MEDIA_STREAM_VIDEO), frame.subarray(MEDIA_FRAME_HEADER, code_offset)).then(function(body)
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
 * @brief splits one encoded frame into packets under the size cap, encrypts each with the channel keys and posts them back for sending
 *
 * @param object data -> epoch, frame_number, is_keyframe, timestamp and the frame bytes (ArrayBuffer)
 *
 * @return void
 */
function video_worker__packetize(data)
{
    if (video_worker_channel_keys == null || video_worker_transport == null || video_worker_transport.session == null)
    {
        global.postMessage({ type: "video_worker__log", value: "no keys yet, frame dropped" });
        return;
    }

    let bytes = new Uint8Array(data.data);
    let piece_size = VIDEO_PACKET_MAX_BYTES - VIDEO_WORKER_HEADER_BYTES;
    let packet_count = Math.max(1, Math.ceil(bytes.length / piece_size));
    let packets = [];
    let timestamp_low = Math.floor(data.timestamp) >>> 0;
    let epoch = data.epoch;

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
        packets.push(packet);
    }

    // the encryption is asynchronous on the browser's crypto; the chain keeps the frames in encoder order
    video_worker_send_chain = video_worker_send_chain.then(function()
    {
        return Promise.all(packets.map(function(packet)
        {
            return video_worker__encrypt_packet(packet);
        }));
    }).then(function(encrypted_packets)
    {
        let buffers = encrypted_packets.map(function(encrypted)
        {
            return (encrypted.byteOffset == 0 && encrypted.byteLength == encrypted.buffer.byteLength) ? encrypted.buffer : encrypted.slice().buffer;
        });

        global.postMessage({ type: "video_worker__packets_ready", epoch: epoch, packets: buffers }, buffers);
    }, function(error)
    {
        global.postMessage({ type: "video_worker__log", value: "frame not encrypted: " + error.message });
    });
}

/**
 * @brief creates the decoder for the announced codec; annex b h.264 and vp8 need no out-of-band description
 *
 * @param string codec -> the webcodecs codec string
 * @param number width -> coded width
 * @param number height -> coded height
 * @param number streamer_client_id -> the client whose packets are decoded; packets of anybody else are dropped
 *
 * @return void
 */
function video_worker__decoder_setup(codec, width, height, streamer_client_id)
{
    video_worker__decoder_close();
    video_worker_streamer_client_id = (typeof streamer_client_id === "number") ? streamer_client_id : -1;

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
 * @brief one packet from the datachannel (sender id already stripped): decrypted, then handed to video_worker__process_packet
 *
 *        the decryption is asynchronous on the browser's AES; the chain keeps the packets in arrival
 *        order. a packet that cannot be decrypted counts as a gap
 *
 * @param ArrayBuffer buffer -> the encrypted packet
 *
 * @return void
 */
function video_worker__decode_packet(buffer)
{
    if (video_worker_channel_keys == null || video_worker_transport == null || video_worker_transport.session == null || video_worker_decoder == null)
    {
        return;
    }

    video_worker_receive_chain = video_worker_receive_chain.then(function()
    {
        return video_worker__decrypt_packet(new Uint8Array(buffer));
    }).then(function(packet)
    {
        if (packet == null)
        {
            video_worker__mark_gap();
            return;
        }
        video_worker__process_packet(packet);
    }, function(error)
    {
        video_worker__mark_gap();
    });
}

/**
 * @brief one decrypted packet: checked, put into the frame being reassembled; a complete frame goes to the decoder
 *        gaps of any kind (a lost packet, a new frame before the old one completed, a decrypt failure
 *        showing as a nonsense header) drop the frame and wait for the next keyframe
 *
 * @param Uint8Array packet -> the decrypted packet, header first
 *
 * @return void
 */
function video_worker__process_packet(packet)
{
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
