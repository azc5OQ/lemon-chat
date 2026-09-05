// Headless receiver for a direct chat FILE sent from a browser client. Plays the phone: node holds
// the connection and must hand its webview (a) the metadata with the header already opened and
// (b) the decrypted body as direct_chat_file_decrypted. Prints one json line per event.
//   node file-receiver-probe.js [attached]     (a server on 1111; send the file from another client)
let t = require("./test-helpers.js");
let crypto = require("crypto");

let is_attached = (process.argv[2] === "attached");

t.start_connected_bundle(function(bundle)
{
    if (is_attached && typeof bundle.node_set_ui_attached === "function")
    {
        bundle.node_set_ui_attached(true);
    }

    bundle.set_frame_listener(function(json_string)
    {
        let frame = null;
        try { frame = JSON.parse(json_string); } catch (e) { return; }
        let type = (frame.message && frame.message.type) ? frame.message.type : "";

        if (type === "direct_chat_file_metadata")
        {
            console.log("PROBE " + JSON.stringify({ type: type, header: frame.message.file_header_decrypted, raw_header_left: frame.message.file_header, encrypted_size: frame.message.encrypted_size }));
        }
        else if (type === "file_receive_chunk" || type === "file_receive_completed")
        {
            if (type === "file_receive_completed") { console.log("PROBE " + JSON.stringify({ type: type, receive_type: frame.message.receive_type })); }
        }
        else if (type === "direct_chat_file_decrypted")
        {
            let file = frame.message.file;
            let bytes = Buffer.from(file.base64, "base64");
            let hash = crypto.createHash("sha256").update(bytes).digest("hex");
            console.log("PROBE " + JSON.stringify({ type: type, name: file.name, size: file.size, mime: file.mime, bytes: bytes.length, sha256: hash }));
            setTimeout(function() { process.exit(0); }, 500);
        }
    });

    console.log("RECEIVER_READY " + bundle.read_state().g_local_username + " attached=" + is_attached);
});

setInterval(function() {}, 60000);
