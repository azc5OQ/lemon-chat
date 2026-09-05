// chat-files.js is a file that is embedded in template.html along with other files
// it facilitates functionality for uploading and receiving all kinds of files from the local filesystem
// its functions are called from three other files in the lemon chat client, chat.js (sending and the upload lock), messages.js (receiving and rendering) and ui.js (attach functionality)
// the encryption mechanism for files in a channel mirrors the channel encryption mechanism of lemon chat; 
// files sent directly to a user mirror the direct chat message encryption mechanism

// above this limit the server settings tab shows a warning, because one transfer holds
// roughly twice the file size in ram
var FILE_UPLOAD_WARNING_MB = 20;

// used only in the worker. it remembers the key set that decrypted a file's header, so the
// body (up to 20 MB) is not tried against every historic key set
var chat_file_keys_by_file_id = {};

// the circumference of the progress ring (radius 15.5). stroke-dashoffset counts down from this value
var CHAT_FILE_RING_LENGTH = 97.4;

// maps file extensions to an icon kind. the kind picks the badge colour of the file icon in layout.style
var CHAT_FILE_KINDS = {
    archive: ["zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz", "iso"],
    pdf: ["pdf"],
    document: ["doc", "docx", "odt", "rtf", "txt", "md", "epub"],
    sheet: ["xls", "xlsx", "ods", "csv"],
    slides: ["ppt", "pptx", "odp"],
    code: ["js", "ts", "py", "c", "h", "cpp", "hpp", "java", "cs", "go", "rs", "php", "rb", "sh", "bat", "html", "css", "json", "xml", "yml", "yaml", "sql"],
    audio: ["mp3", "wav", "ogg", "flac", "m4a", "aac", "opus", "wma"],
    video: ["mp4", "mkv", "avi", "mov", "webm", "wmv", "m4v"],
    image: ["png", "jpg", "jpeg", "gif", "ico", "bmp", "webp", "svg", "tiff", "heic"],
    executable: ["exe", "msi", "apk", "dmg", "deb", "rpm", "appimage"]
};

// the extensions the picture picker accepts. anything else dropped on the chat is sent as a file
var CHAT_PICTURE_EXTENSIONS = ["png", "jpg", "jpeg", "gif", "ico", "webp", "bmp", "svg", "avif"];

// ---------------------------------------------------------------------------
// base64 and crypto helpers. these run in the page, in the workers and in the node client
// ---------------------------------------------------------------------------

/**
 * @brief bytes -> base64 with the native btoa, about 100 times faster than utils__bytesToBase64String, which matters at 20 MB
 *
 * @param Uint8Array bytes -> the bytes
 *
 * @return string the base64 text
 */
function chat_files__fast_bytes_to_base64(bytes)
{
    if (typeof btoa !== "function")
    {
        return utils__bytesToBase64String(bytes);
    }

    let chunks = [];
    const step = 0x8000;

    for (let i = 0; i < bytes.length; i += step)
    {
        chunks.push(String.fromCharCode.apply(null, bytes.subarray(i, i + step)));
    }

    return btoa(chunks.join(""));
}

/**
 * @brief base64 -> bytes with the native atob; throws on malformed base64, because atob does, and the callers catch that
 *
 * @param string base64_string -> the base64 text
 *
 * @return Uint8Array the bytes
 */
function chat_files__fast_base64_to_bytes(base64_string)
{
    if (typeof atob !== "function")
    {
        return new Uint8Array(utils__base64StringToBytes(base64_string));
    }

    let binary = atob(base64_string);
    let bytes = new Uint8Array(binary.length);

    for (let i = 0; i < binary.length; i++)
    {
        bytes[i] = binary.charCodeAt(i);
    }

    return bytes;
}

/**
 * @brief encrypts a string the way pictures are encrypted: utf8 bytes, zero padding, aes-ctr with every key, then base64
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string string_to_encrypt -> the plaintext
 * @param number minimum_padded_bytes -> the padding floor; defaults to the picture path's 1024, file bodies pass CHAT_FILE_BODY_MIN_PADDED_BYTES to clear the server's minimum upload size
 *
 * @return string the base64 ciphertext
 */
function chat_files__encrypt_string_with_aes_keys_fast(keys, string_to_encrypt, minimum_padded_bytes)
{
    let padding_floor = (typeof minimum_padded_bytes === "number") ? minimum_padded_bytes : 1024;
    let bytes = aesjs.utils.utf8.toBytes(string_to_encrypt);

    if (bytes.length < padding_floor)
    {
        let padded = new Uint8Array(padding_floor);
        padded.set(bytes);
        bytes = padded;
    }

    let data = bytes;

    for (let i = 0; i < keys.length; i++)
    {
        let aes_ctr = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        data = aes_ctr.encrypt(data);
    }

    return chat_files__fast_bytes_to_base64(data);
}

/**
 * @brief decrypts what chat_files__encrypt_string_with_aes_keys_fast produced
 *
 * @param array keys -> the key layers, each with key_bytes and iv_bytes
 * @param string base64_string -> the base64 ciphertext
 *
 * @return string|null the plaintext cut at the first null, null when the base64 is malformed
 */
function chat_files__decrypt_base64_with_aes_keys_fast(keys, base64_string)
{
    let data = null;

    try
    {
        data = chat_files__fast_base64_to_bytes(base64_string);
    }
    catch (decode_error)
    {
        return null;
    }

    for (let i = keys.length - 1; i >= 0; i--)
    {
        let aes_ctr = new aesjs.ModeOfOperation.ctr(keys[i].key_bytes, new aesjs.Counter(keys[i].iv_bytes));
        data = aes_ctr.decrypt(data);
    }

    return utils__substringByNullTerminator(new TextDecoder().decode(data));
}

/**
 * @brief four fresh random aes-ctr keys, the key set a direct message envelope uses
 *
 * @return array the keys as [{ key_bytes, iv_bytes }]
 */
function chat_files__create_random_message_keys()
{
    let keys = [];

    for (let i = 0; i < 4; i++)
    {
        keys.push({
            key_bytes: Array.from(crypto.getRandomValues(new Uint8Array(32))),
            iv_bytes: Array.from(crypto.getRandomValues(new Uint8Array(16)))
        });
    }

    return keys;
}

/**
 * @brief builds the direct chat envelope: message_keys carries the aes keys encrypted with the receiver's rsa key, message_value the aes-encrypted plaintext
 *
 * @param string receiver_public_key -> the receiver's public key string
 * @param string plaintext -> the text to wrap
 * @param number minimum_padded_bytes -> the padding floor, see chat_files__encrypt_string_with_aes_keys_fast
 *
 * @return string|null the envelope as json, null when the receiver's public key is unusable
 */
function chat_files__create_direct_chat_file_envelope(receiver_public_key, plaintext, minimum_padded_bytes)
{
    let keys = chat_files__create_random_message_keys();
    let encryption_result = lemon_crypto.encrypt(JSON.stringify(keys), receiver_public_key);

    if (encryption_result == null || encryption_result.status != "success")
    {
        return null;
    }

    return JSON.stringify({
        message_keys: encryption_result.cipher,
        message_value: chat_files__encrypt_string_with_aes_keys_fast(keys, plaintext, minimum_padded_bytes)
    });
}

/**
 * @brief opens a direct envelope with our own rsa key
 *
 * @param string envelope_json_string -> the envelope as json
 *
 * @return string|null the plaintext, null when anything about the envelope is wrong
 */
function chat_files__open_direct_chat_file_envelope(envelope_json_string)
{
    let envelope = null;

    try
    {
        envelope = JSON.parse(envelope_json_string);
    }
    catch (parse_error)
    {
        return null;
    }

    if (envelope == null || typeof envelope.message_keys !== "string" || typeof envelope.message_value !== "string")
    {
        return null;
    }

    let decryption_result = lemon_crypto.decrypt(envelope.message_keys, g_my_rsa_key_object);

    if (decryption_result.status == "failure")
    {
        return null;
    }

    let keys = null;

    try
    {
        keys = JSON.parse(decryption_result.plaintext);
    }
    catch (keys_error)
    {
        return null;
    }

    return chat_files__decrypt_base64_with_aes_keys_fast(keys, envelope.message_value);
}

/**
 * @brief the channel key sets a file could be encrypted with: the hinted set first, then the current channel keys, then the historic ones
 *
 * @param array|null hinted_keys -> the set that worked before, or null
 *
 * @return array the key sets to try, in that order
 */
function chat_files__get_channel_key_candidates(hinted_keys)
{
    let candidates = [];

    if (hinted_keys != null)
    {
        candidates.push(hinted_keys);
    }

    if (g_current_channel_keys != null && g_current_channel_keys !== hinted_keys)
    {
        candidates.push(g_current_channel_keys);
    }

    for (let i = 0; i < g_historic_keys_of_current_channel.length; i++)
    {
        if (g_historic_keys_of_current_channel[i] !== hinted_keys)
        {
            candidates.push(g_historic_keys_of_current_channel[i]);
        }
    }

    return candidates;
}

/**
 * @brief decrypts the header of a channel file, trying every candidate key set, and remembers the one that worked so the body can use it directly
 *        runs in the worker
 *
 * @param number file_id -> the transfer
 * @param string header_base64 -> the encrypted header
 *
 * @return object|null { name, size, mime }, null when no key set fits
 */
function chat_files__decrypt_channel_chat_file_header(file_id, header_base64)
{
    let candidates = chat_files__get_channel_key_candidates(null);

    for (let i = 0; i < candidates.length; i++)
    {
        let header = chat_files__parse_chat_file_header(chat_files__decrypt_base64_with_aes_keys_fast(candidates[i], header_base64));

        if (header != null)
        {
            chat_file_keys_by_file_id[file_id] = candidates[i];
            return header;
        }
    }

    return null;
}

/**
 * @brief decrypts the body of a channel file with the key set the header found, else every candidate
 *        runs in the worker
 *
 * @param number file_id -> the transfer
 * @param string body_base64 -> the encrypted body
 *
 * @return object|null { name, size, mime, base64 }, null when no key set fits
 */
function chat_files__decrypt_channel_chat_file_body(file_id, body_base64)
{
    let candidates = chat_files__get_channel_key_candidates(chat_file_keys_by_file_id[file_id]);

    delete chat_file_keys_by_file_id[file_id];

    for (let i = 0; i < candidates.length; i++)
    {
        let file = chat_files__parse_decrypted_chat_file_body(chat_files__decrypt_base64_with_aes_keys_fast(candidates[i], body_base64));

        if (file != null)
        {
            return file;
        }
    }

    return null;
}

// ---------------------------------------------------------------------------
// the shapes that travel over the wire
// ---------------------------------------------------------------------------

/**
 * @brief the header json that travels encrypted in the metadata; it carries enough to draw the file card before the body arrives
 *
 * @param object file -> { name, size, mime }
 *
 * @return string the json
 */
function chat_files__build_chat_file_header_json(file)
{
    return JSON.stringify({ name: file.name, size: file.size, mime: file.mime });
}

/**
 * @brief the body json that travels encrypted in the 400-part upload
 *
 * @param object file -> { name, size, mime, base64 }
 *
 * @return string the json
 */
function chat_files__build_chat_file_body_json(file)
{
    return JSON.stringify({ name: file.name, size: file.size, mime: file.mime, data: file.base64 });
}

/**
 * @brief parses a decrypted header or body, sanitising the name and the mime type
 *        decrypting with a wrong key produces noise instead of json, so anything that is not one is null
 *
 * @param string text -> the decrypted text
 *
 * @return object|null the parsed object, null when the text is not a file json
 */
function chat_files__parse_chat_file_json(text)
{
    if (typeof text !== "string" || text.charAt(0) !== "{")
    {
        return null;
    }

    let parsed = null;

    try
    {
        parsed = JSON.parse(text);
    }
    catch (parse_error)
    {
        return null;
    }

    if (parsed == null || typeof parsed.name !== "string")
    {
        return null;
    }

    return {
        name: chat_files__sanitize_file_name(parsed.name),
        size: (typeof parsed.size === "number" && parsed.size > 0) ? Math.floor(parsed.size) : 0,
        mime: chat_files__sanitize_mime_type(parsed.mime),
        data: parsed.data
    };
}

/**
 * @brief a decrypted header as { name, size, mime }
 *
 * @param string text -> the decrypted text
 *
 * @return object|null the header, null when the text is not one
 */
function chat_files__parse_chat_file_header(text)
{
    let parsed = chat_files__parse_chat_file_json(text);

    if (parsed == null)
    {
        return null;
    }

    return { name: parsed.name, size: parsed.size, mime: parsed.mime };
}

/**
 * @brief a decrypted body as { name, size, mime, base64 }, the data checked to be base64
 *
 * @param string text -> the decrypted text
 *
 * @return object|null the body, null when the text is not one
 */
function chat_files__parse_decrypted_chat_file_body(text)
{
    let parsed = chat_files__parse_chat_file_json(text);

    if (parsed == null || typeof parsed.data !== "string" || /^[A-Za-z0-9+/]*={0,2}$/.test(parsed.data) == false)
    {
        return null;
    }

    return { name: parsed.name, size: parsed.size, mime: parsed.mime, base64: parsed.data };
}

// ---------------------------------------------------------------------------
// names, sizes, icons
// ---------------------------------------------------------------------------

/**
 * @brief cleans a file name: no path separators or control characters, at most 255 characters, never empty
 *
 * @param string name -> the name as received
 *
 * @return string the cleaned name, "file" when nothing usable is left
 */
function chat_files__sanitize_file_name(name)
{
    let result = ("" + (name == null ? "" : name)).replace(/[\\\/\x00-\x1f\x7f]/g, "_").trim();

    if (result.length > 255)
    {
        result = result.slice(0, 255);
    }

    if (result.length == 0 || result == "." || result == "..")
    {
        result = "file";
    }

    return result;
}

/**
 * @brief cleans a mime type: lowercase, at most 100 characters, of the type/subtype shape
 *
 * @param string mime -> the type as received
 *
 * @return string the cleaned type, "application/octet-stream" when it is not a valid one
 */
function chat_files__sanitize_mime_type(mime)
{
    let text = "" + (mime == null ? "" : mime);

    if (/^[a-z0-9!#$&^_.+-]+\/[a-z0-9!#$&^_.+-]+$/i.test(text) == false || text.length > 100)
    {
        return "application/octet-stream";
    }

    return text.toLowerCase();
}

/**
 * @brief the icon kind of a file extension, from CHAT_FILE_KINDS
 *
 * @param string extension -> the extension without the dot
 *
 * @return string the kind, "generic" when unknown
 */
function chat_files__get_chat_file_kind(extension)
{
    for (let kind in CHAT_FILE_KINDS)
    {
        if (CHAT_FILE_KINDS[kind].indexOf(extension) != -1)
        {
            return kind;
        }
    }

    return "generic";
}

/**
 * @brief whether a file takes the picture path
 *        the browser's own type is trusted first, so a dropped image behaves like one picked from
 *        disk; the extension is the fallback for a drop that carried no type
 *
 * @param File file -> the file
 *
 * @return boolean true for a picture
 */
function chat_files__is_chat_picture_file(file)
{
    if (file == null)
    {
        return false;
    }

    // the browser's own type is trusted first, so a dropped image behaves like one picked from
    // disk. the extension is the fallback for a drop that carried no type
    if (typeof file.type === "string" && file.type.indexOf("image/") === 0)
    {
        return true;
    }

    return CHAT_PICTURE_EXTENSIONS.indexOf(chat__get_file_extension(file.name)) != -1;
}

/**
 * @brief a byte count as "12 KB" / "1.5 MB" text
 *
 * @param number bytes -> the size
 *
 * @return string the text, "" for anything that is not a non-negative number
 */
function chat_files__format_file_size(bytes)
{
    if (typeof bytes !== "number" || bytes < 0)
    {
        return "";
    }

    if (bytes < 1024)
    {
        return bytes + " B";
    }

    if (bytes < 1024 * 1024)
    {
        return (bytes / 1024).toFixed(bytes < 10 * 1024 ? 1 : 0) + " KB";
    }

    if (bytes < 1024 * 1024 * 1024)
    {
        return (bytes / (1024 * 1024)).toFixed(bytes < 10 * 1024 * 1024 ? 2 : 1) + " MB";
    }

    return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB";
}

/**
 * @brief the file icon: a page with a folded corner and, for a known extension, a coloured badge carrying it
 *
 * @param string file_name -> the name the extension is taken from
 * @param string css_class -> the class of the svg
 *
 * @return string the svg markup
 */
function chat_files__generate_chat_file_icon_svg(file_name, css_class)
{
    let extension = chat__get_file_extension(file_name);
    let label = (extension.length > 0 && extension.length <= 4) ? extension.toUpperCase() : "";
    let badge = "";

    if (label.length > 0)
    {
        badge = "<rect class=\"chat-file-icon-badge\" x=\"1\" y=\"27\" width=\"38\" height=\"16\" rx=\"3\"></rect>"
              + "<text class=\"chat-file-icon-text\" x=\"20\" y=\"38.5\" text-anchor=\"middle\">" + label + "</text>";
    }

    return "<svg class=\"" + css_class + " chat-file-kind-" + chat_files__get_chat_file_kind(extension) + "\" viewBox=\"0 0 40 48\" aria-hidden=\"true\">"
         + "<path class=\"chat-file-icon-page\" d=\"M7 1h18l11 11v32a3 3 0 0 1-3 3H7a3 3 0 0 1-3-3V4a3 3 0 0 1 3-3z\"></path>"
         + "<path class=\"chat-file-icon-fold\" d=\"M25 1v11h11\"></path>"
         + badge
         + "</svg>";
}

// ---------------------------------------------------------------------------
// the file card in the chat
// ---------------------------------------------------------------------------

/**
 * @brief the file card html
 *
 * @param object options -> { key, name, size, is_local, local_message_id, is_receiving }; key is the server message id for received files, or "local-N" for our own echo
 *
 * @return string the card markup
 */
function chat_files__generate_chat_file_card_html(options)
{
    let name_html = chat__sanitize_string(options.name);
    let size_text = (options.size > 0) ? chat_files__format_file_size(options.size) : "";
    let classes = "chat-file-card";
    let attributes = " data-chat-file-key=\"" + options.key + "\"";

    if (options.is_local == true)
    {
        classes += " local-client-chat-file imgnotsentyet";
        attributes += " data-single-chat-message-local-message-id=\"" + options.local_message_id + "\" data-single-chat-message-server-message-id=\"unspecified\"";
    }
    else
    {
        attributes += " id=\"chat-file-card-" + options.key + "\" data-single-chat-message-server-message-id=\"" + options.key + "\"";
    }

    if (options.is_receiving == true)
    {
        classes += " chat-file-receiving";
    }

    return "<div class=\"" + classes + "\"" + attributes + ">"
         + "<div class=\"chat-file-icon-wrap\">" + chat_files__generate_chat_file_icon_svg(options.name, "chat-file-icon") + "</div>"
         + "<div class=\"chat-file-info\">"
         + "<p class=\"chat-file-name\" title=\"" + name_html + "\">" + name_html + "</p>"
         + "<p class=\"chat-file-size\">" + size_text + "</p>"
         + "<p class=\"chat-file-status\"></p>"
         + "</div>"
         + "<div class=\"chat-file-progress\" title=\"receiving\">"
         + "<svg viewBox=\"0 0 36 36\">"
         + "<circle class=\"chat-file-ring-track\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle>"
         + "<circle class=\"chat-file-ring-bar\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle></svg>"
         + "<span class=\"chat-file-progress-text\">0%</span>"
         + "</div>"
         + "<button type=\"button\" class=\"chat-file-download\" title=\"download\"></button>"
         + "</div>";
}

/**
 * @brief finds a file card or an incoming picture's progress ring by its key; both carry the same data attribute and the same ring innards, so one lookup serves both
 *
 * @param string|number key -> the transfer key
 *
 * @return Element|null the element, null when there is none
 */
function chat_files__get_chat_file_card_by_key(key)
{
    return document.querySelector('[data-chat-file-key="' + key + '"]');
}

/**
 * @brief registers an incoming transfer with its total size, so its progress ring can be moved
 *
 * @param number file_id -> the transfer
 * @param number encrypted_size -> the encrypted size in characters, 0 when unknown
 *
 * @return void
 */
function chat_files__begin_chat_file_transfer(file_id, encrypted_size)
{
    g_chat_file_transfers[file_id] = {
        encrypted_size: (typeof encrypted_size === "number" && encrypted_size > 0) ? encrypted_size : 0
    };
}

/**
 * @brief the progress ring a file card shows, for an incoming picture, because a picture has no card of its own
 *
 * @param number picture_id -> the transfer
 *
 * @return string the ring markup
 */
function chat_files__generate_chat_picture_progress_html(picture_id)
{
    return "<div class=\"chat-picture-progress\" data-chat-file-key=\"" + picture_id + "\">"
         + "<div class=\"chat-picture-progress-ring\">"
         + "<svg viewBox=\"0 0 36 36\">"
         + "<circle class=\"chat-file-ring-track\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle>"
         + "<circle class=\"chat-file-ring-bar\" cx=\"18\" cy=\"18\" r=\"15.5\"></circle></svg>"
         + "<span class=\"chat-file-progress-text\">0%</span>"
         + "</div>"
         + "<span class=\"chat-picture-progress-label\">receiving image ...</span>"
         + "</div>";
}

/**
 * @brief removes the progress ring and forgets the transfer, once the picture arrived or its transfer died
 *
 * @param number picture_id -> the transfer
 *
 * @return void
 */
function chat_files__finish_chat_picture_progress(picture_id)
{
    delete g_chat_file_transfers[picture_id];

    let ring = document.querySelector('.chat-picture-progress[data-chat-file-key="' + picture_id + '"]');

    if (ring != null)
    {
        ring.remove();
    }
}

/**
 * @brief moves the progress ring from how many characters of the transfer arrived so far
 *
 * @param number file_id -> the transfer
 * @param number received_chars -> characters received so far
 *
 * @return void
 */
function chat_files__update_chat_file_progress(file_id, received_chars)
{
    let transfer = g_chat_file_transfers[file_id];

    if (transfer == null)
    {
        return;
    }

    let ratio = (transfer.encrypted_size > 0) ? Math.min(1, received_chars / transfer.encrypted_size) : 0;
    chat_files__render_chat_file_progress(file_id, ratio);
}

/**
 * @brief paints a ratio onto a transfer's ring and its percentage text
 *
 * @param number file_id -> the transfer
 * @param number ratio -> 0 to 1
 *
 * @return void
 */
function chat_files__render_chat_file_progress(file_id, ratio)
{
    let card = chat_files__get_chat_file_card_by_key(file_id);

    if (card == null)
    {
        return;
    }

    let bar = card.querySelector(".chat-file-ring-bar");
    let text = card.querySelector(".chat-file-progress-text");

    if (bar != null)
    {
        bar.style.strokeDashoffset = (CHAT_FILE_RING_LENGTH * (1 - ratio)).toFixed(2);
    }

    if (text != null)
    {
        text.textContent = Math.round(ratio * 100) + "%";
    }
}

/**
 * @brief enables a card's download button and points it at the stored file
 *
 * @param Element card -> the file card
 * @param string|number key -> the transfer key
 *
 * @return void
 */
function chat_files__wire_chat_file_download_button(card, key)
{
    let button = card.querySelector(".chat-file-download");

    if (button == null)
    {
        return;
    }

    button.disabled = false;
    button.onclick = function(event)
    {
        event.stopPropagation();
        chat_files__download_chat_file(key);
    };
}

/**
 * @brief finishes a file card once the body arrived and decrypted: the file is kept for the download button and the progress ring goes away
 *        when there is no live card, no copy is kept; this catches node's dead elements (the phone
 *        would otherwise hold every received file in ram forever), a cleared chat and a deleted message
 *
 * @param string|number key -> the transfer key
 * @param object file -> { name, size, mime, base64 }
 *
 * @return void
 */
function chat_files__finish_chat_file_card(key, file)
{
    delete g_chat_file_transfers[key];

    let card = chat_files__get_chat_file_card_by_key(key);

    // when there is no live card, no copy is kept. this catches node's dead elements (the phone
    // would otherwise hold every received file in ram forever), a cleared chat and a deleted message
    if (card == null || card.isConnected !== true || card.classList.contains("chat-file-deleted"))
    {
        return;
    }

    g_chat_files_by_message_id[key] = file;
    card.classList.remove("chat-file-receiving");

    let name = card.querySelector(".chat-file-name");
    let size = card.querySelector(".chat-file-size");

    if (name != null)
    {
        name.textContent = file.name;
        name.title = file.name;
    }

    if (size != null)
    {
        size.textContent = chat_files__format_file_size(file.size > 0 ? file.size : Math.floor(file.base64.length * 3 / 4));
    }

    chat_files__wire_chat_file_download_button(card, key);
}

/**
 * @brief turns a card into its failed state with a status text
 *
 * @param Element|null card -> the file card, nothing happens for null
 * @param string text -> the status text
 *
 * @return void
 */
function chat_files__mark_chat_file_card_failed(card, text)
{
    if (card == null)
    {
        return;
    }

    card.classList.remove("imgnotsentyet");
    card.classList.remove("chat-file-receiving");
    card.classList.add("chat-file-failed");

    let status = card.querySelector(".chat-file-status");

    if (status != null)
    {
        status.textContent = text;
    }
}

/**
 * @brief marks a refused upload as failed
 *        the server names the local message id it refused; 0 means it refused the very first part,
 *        and then the newest unsent card is the one that failed
 *
 * @param number g_local_message_id -> the refused local message id, 0 for the newest unsent card
 * @param string text -> the status text
 *
 * @return void
 */
function chat_files__mark_local_chat_file_card_failed(g_local_message_id, text)
{
    let card = null;

    if (typeof g_local_message_id === "number" && g_local_message_id > 0)
    {
        card = document.querySelector('.local-client-chat-file[data-single-chat-message-local-message-id="' + g_local_message_id + '"]');
    }
    else
    {
        let unsent = document.querySelectorAll(".local-client-chat-file.imgnotsentyet");
        card = (unsent.length > 0) ? unsent[unsent.length - 1] : null;
    }

    chat_files__mark_chat_file_card_failed(card, text);
}

/**
 * @brief turns a card into its deleted state: forgets the file and the transfer, unwires the menu, leaves a "deleted" card
 *
 * @param Element|null card -> the file card, nothing happens for null
 *
 * @return void
 */
function chat_files__mark_chat_file_card_deleted(card)
{
    if (card == null)
    {
        return;
    }

    delete g_chat_files_by_message_id[card.getAttribute("data-chat-file-key")];
    delete g_chat_file_transfers[card.getAttribute("data-chat-file-key")];
    card.removeEventListener("mousedown", chat_files__chat_file_card_onrightclick);
    card.className = "chat-file-card chat-file-deleted";
    card.innerHTML = "<p class=\"chat-file-status\">deleted</p>";
}

/**
 * @brief forgets the stored bytes of cards that no longer exist, after "clear chat" removed them
 *
 * @return void
 */
function chat_files__prune_chat_files_without_cards()
{
    for (let key in g_chat_files_by_message_id)
    {
        if (chat_files__get_chat_file_card_by_key(key) == null)
        {
            delete g_chat_files_by_message_id[key];
        }
    }
}

/**
 * @brief the text for a refused file: uploads off, too big, receiver unavailable, private messages off, or the server's own reason
 *
 * @param string reason -> the reason code from the server
 * @param number max_bytes -> the server's limit, for the "too big" text
 *
 * @return string the text to show
 */
function chat_files__explain_file_send_error(reason, max_bytes)
{
    if (reason == "file_uploads_disabled")
    {
        return "file not sent: file uploads are not allowed on this server";
    }

    if (reason == "file_too_large")
    {
        return "file not sent: too big, this server allows up to " + chat_files__format_file_size(max_bytes);
    }

    if (reason == "receiver_unavailable")
    {
        return "file not sent: the receiver is not available (offline or idle)";
    }

    if (reason == "private_messages_disabled")
    {
        return "file not sent: private messages are disabled on this server";
    }

    return "file not sent: refused by the server (" + chat__sanitize_string("" + reason) + ")";
}

/**
 * @brief right click on a file card opens the delete menu; there is no edit item, because a file has nothing to edit
 *
 * @param object event -> the mouse event
 *
 * @return void
 */
function chat_files__chat_file_card_onrightclick(event)
{
    if (event.which != 3)
    {
        return;
    }

    let card = event.currentTarget;
    let server_message_id = card.getAttribute("data-single-chat-message-server-message-id");

    if (server_message_id == null || server_message_id == "unspecified")
    {
        return;
    }

    event.stopPropagation();

    let old_menus = document.getElementsByClassName("chat-message-context-menu");

    while (old_menus.length > 0)
    {
        old_menus[0].remove();
    }

    g_selected_server_chat_message_id = parseInt(server_message_id);

    let contextmenu_html = "<div class=\"chat-message-context-menu\" style=\"top: " + event.clientY + "px; left: " + event.clientX + "px;\">"
                         + "<div class='chat-message-context-menu-background'></div>"
                         + "<div class='chat-message-context-menu-items'>"
                         + "<p class='chat-message-context-menu-item' data-action='0'>delete</p>"
                         + "</div></div>";

    document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", contextmenu_html);

    let items = document.getElementsByClassName("chat-message-context-menu-item");

    for (let i = 0; i < items.length; i++)
    {
        items[i].addEventListener("mousedown", UI.chat_message_contextmenuitem_onclick);
    }
}

// ---------------------------------------------------------------------------
// download
// ---------------------------------------------------------------------------

/**
 * @brief saves a received file: the java side writes it into Downloads in the webview, a browser gets a blob download
 *
 * @param string|number key -> the transfer key
 *
 * @return void
 */
function chat_files__download_chat_file(key)
{
    let file = g_chat_files_by_message_id[key];

    if (file == null)
    {
        utils__custom_alert("this file is no longer available");
        return;
    }

    // the android webview can not save a blob url, so the java side writes the file into Downloads instead
    if (typeof Android !== "undefined" && Android != null && typeof Android.JavaExportSaveFile === "function")
    {
        Android.JavaExportSaveFile(file.name, file.mime, file.base64);
        return;
    }

    let bytes = null;

    try
    {
        bytes = chat_files__fast_base64_to_bytes(file.base64);
    }
    catch (decode_error)
    {
        utils__custom_alert("the file data is damaged, it can not be saved");
        return;
    }

    let url = URL.createObjectURL(new Blob([bytes], { type: file.mime }));
    let anchor = document.createElement("a");

    anchor.href = url;
    anchor.download = file.name;
    anchor.style.display = "none";
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();

    setTimeout(function() { URL.revokeObjectURL(url); }, 60000);
}

// ---------------------------------------------------------------------------
// attaching: paperclip, drag & drop, the pending chip
// ---------------------------------------------------------------------------

/**
 * @brief the drag and drop entry point; every refusal explains why
 *        a dropped image takes the picture path; an image over the server's inline picture limit
 *        goes out as a plain file instead
 *
 * @param File file -> the dropped file
 *
 * @return void
 */
function chat_files__attach_chat_file_or_picture(file)
{
    if (file == null)
    {
        return;
    }

    if (document.getElementById("chat-input-container-text-input").disabled == true)
    {
        utils__custom_alert("join a channel or open a private chat first");
        return;
    }

    if (chat_files__is_chat_picture_file(file))
    {
        if (g_server_policy.allow_chat_pictures == false)
        {
            if (g_server_policy.allow_file_uploads == true && file.size <= g_server_policy.file_upload_max_size)
            {
                utils__custom_alert("inline pictures are disabled on this server, so the image goes as a file");
                chat_files__attach_chat_file(file);
                return;
            }

            utils__custom_alert("inline pictures are disabled on this server");
            return;
        }

        if (file.size <= g_server_policy.chat_picture_max_size)
        {
            chat_files__attach_chat_picture(file);
            return;
        }

        if (g_server_policy.allow_file_uploads == true && file.size <= g_server_policy.file_upload_max_size)
        {
            utils__custom_alert("image is over the server's " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + " picture limit, so it goes as a file - the receiver downloads it instead of seeing it inline");
            chat_files__attach_chat_file(file);
            return;
        }

        utils__custom_alert("image is too large to send as a picture (this server allows up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + ")");
        return;
    }

    chat_files__attach_chat_file(file);
}

/**
 * @brief attaches a picture for the next send, the same preview path the picture button uses; drag and drop lands here too
 *
 * @param File file -> the picture
 *
 * @return void
 */
function chat_files__attach_chat_picture(file)
{
    if (g_server_policy.allow_chat_pictures == false)
    {
        utils__custom_alert("inline pictures are disabled on this server");
        return;
    }

    if (file.size > g_server_policy.chat_picture_max_size)
    {
        if (g_server_policy.allow_file_uploads == true && file.size <= g_server_policy.file_upload_max_size)
        {
            utils__custom_alert("image is too large to send as a picture (this server allows up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + "). use the paperclip to send it as a file");
        }
        else
        {
            utils__custom_alert("image is too large to send as a picture (this server allows up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + ")");
        }

        return;
    }

    let reader = new FileReader();

    reader.onload = function(event)
    {
        // one attachment per message, so a pending file gives way to the picture
        chat_files__clear_pending_chat_file();

        // the thumbnail overlays the text box, so the typed text is cleared out of its way
        document.getElementById("chat-input-container-text-input").value = "";
        document.getElementById("image-upload-preview").style.aspectRatio = "";
        document.getElementById("image-upload-preview").style.display = "inline-block";
        document.getElementById("image-upload-preview").style.backgroundImage = "url(" + event.target.result + ")";
        g_base64_picture_string_to_send = event.target.result;

        // the thumbnail takes the picture's own shape, because the css box alone would crop it
        let preview_image = new Image();
        preview_image.onload = function()
        {
            if (preview_image.naturalWidth > 0 && preview_image.naturalHeight > 0 && g_base64_picture_string_to_send === preview_image.src)
            {
                document.getElementById("image-upload-preview").style.aspectRatio = preview_image.naturalWidth + " / " + preview_image.naturalHeight;
            }
        };
        preview_image.src = event.target.result;
    };

    reader.onerror = function()
    {
        utils__custom_alert("could not read the image");
    };

    reader.readAsDataURL(file);
}

/**
 * @brief attaches a file for the next send after the policy checks (uploads allowed, within the size limit, not empty); it replaces a pending picture and shows the chip
 *
 * @param File file -> the file
 *
 * @return void
 */
function chat_files__attach_chat_file(file)
{
    if (g_server_policy.allow_file_uploads == false)
    {
        utils__custom_alert("file uploads are not allowed on this server");
        return;
    }

    if (file.size > g_server_policy.file_upload_max_size)
    {
        utils__custom_alert("file is too big (" + chat_files__format_file_size(file.size) + "). this server allows up to " + chat_files__format_file_size(g_server_policy.file_upload_max_size));
        return;
    }

    if (file.size == 0)
    {
        utils__custom_alert("the file is empty");
        return;
    }

    let reader = new FileReader();

    reader.onload = function(event)
    {
        g_pending_chat_file = {
            name: chat_files__sanitize_file_name(file.name),
            size: file.size,
            mime: chat_files__sanitize_mime_type(file.type),
            base64: chat__remove_data_url_prefix_from_base64_string(event.target.result)
        };

        // one attachment per message, so a pending picture gives way
        chat_files__clear_pending_chat_picture();

        // the chip overlays the text box, so the typed text is cleared out of its way
        document.getElementById("chat-input-container-text-input").value = "";

        chat_files__render_pending_chat_file_chip();
    };

    reader.onerror = function()
    {
        utils__custom_alert("could not read the file");
    };

    reader.readAsDataURL(file);
}

/**
 * @brief drops the pending file and hides its chip
 *
 * @return void
 */
function chat_files__clear_pending_chat_file()
{
    g_pending_chat_file = null;
    chat_files__render_pending_chat_file_chip();
}

/**
 * @brief removes a pending picture: the thumbnail hides and nothing is left to send
 *        wired to the thumbnail's cancel cross, and used by the file path because one attachment replaces the other
 *
 * @return void
 */
function chat_files__clear_pending_chat_picture()
{
    g_base64_picture_string_to_send = "";
    document.getElementById("image-upload-preview").style.backgroundImage = "";
    document.getElementById("image-upload-preview").style.display = "none";
    document.getElementById("image-upload-preview").style.aspectRatio = "";
}

/**
 * @brief renders the pending file chip over the text box's corner, or nothing when no file is pending
 *
 * @return void
 */
function chat_files__render_pending_chat_file_chip()
{
    let chip = document.getElementById("file-upload-preview");

    if (chip == null)
    {
        return;
    }

    if (g_pending_chat_file == null)
    {
        chip.style.display = "none";
        chip.innerHTML = "";
        return;
    }

    chip.innerHTML = chat_files__generate_chat_file_icon_svg(g_pending_chat_file.name, "file-upload-preview-icon")
                   + "<span class=\"file-upload-preview-text\">"
                   + "<span class=\"file-upload-preview-name\">" + chat__sanitize_string(g_pending_chat_file.name) + "</span>"
                   + "<span class=\"file-upload-preview-size\">" + chat_files__format_file_size(g_pending_chat_file.size) + "</span>"
                   + "</span>"
                   + "<span class=\"file-upload-preview-remove\" title=\"remove\">&#10005;</span>";

    chip.querySelector(".file-upload-preview-remove").onclick = function(event)
    {
        event.stopPropagation();
        chat_files__clear_pending_chat_file();
    };

    chip.style.display = "flex";
}

/**
 * @brief the paperclip's file input
 *        it always attaches as a plain file, even an image, because the user chose the file button
 *        on purpose; the picture path belongs to the image button and drag and drop
 *
 * @return void
 */
function chat_files__choose_chat_file_input_onchange()
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    let files = document.getElementById("choose-file-input").files;

    if (files.length > 0)
    {
        chat_files__attach_chat_file(files[0]);
    }
}

/**
 * @brief sets up drag and drop: the text box and the chat itself take drops; the document refuses them, because a missed drop would otherwise make the browser open the file as a page
 *
 * @return void
 */
function chat_files__setup_chat_file_drag_and_drop()
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    document.addEventListener("dragover", function(event) { event.preventDefault(); });
    document.addEventListener("drop", function(event) { event.preventDefault(); });

    let zone_ids = ["chat-input-file-input-container", "chat-context-container"];

    for (let i = 0; i < zone_ids.length; i++)
    {
        let zone = document.getElementById(zone_ids[i]);

        if (zone == null)
        {
            continue;
        }

        // dragenter and dragleave fire for every child element; the depth counter turns them into one highlight
        let depth = 0;

        zone.addEventListener("dragenter", function(event)
        {
            event.preventDefault();
            depth++;
            zone.classList.add("chat-drop-active");
        });

        zone.addEventListener("dragleave", function()
        {
            depth--;

            if (depth <= 0)
            {
                depth = 0;
                zone.classList.remove("chat-drop-active");
            }
        });

        zone.addEventListener("dragover", function(event)
        {
            event.preventDefault();
            event.dataTransfer.dropEffect = "copy";
        });

        zone.addEventListener("drop", function(event)
        {
            event.preventDefault();
            depth = 0;
            zone.classList.remove("chat-drop-active");

            let files = (event.dataTransfer != null) ? event.dataTransfer.files : null;

            if (files == null || files.length == 0)
            {
                return;
            }

            if (files.length > 1)
            {
                utils__custom_alert("one file at a time - taking the first one");
            }

            chat_files__attach_chat_file_or_picture(files[0]);
        });
    }
}

/**
 * @brief the cursor-lit border of the file cards: one delegated mousemove on the chat writes the cursor position into the hovered card's --mx and --my, which the default theme's ring gradient follows
 *
 * @return void
 */
function chat_files__setup_chat_file_card_glow()
{
    if (G_HAS_DOM == false)
    {
        return;
    }

    let chat = document.getElementById("chat-context-container");

    if (chat == null)
    {
        return;
    }

    chat.addEventListener("mousemove", function(event)
    {
        let card = (event.target != null && typeof event.target.closest === "function") ? event.target.closest(".chat-file-card") : null;

        if (card == null)
        {
            return;
        }

        let box = card.getBoundingClientRect();
        card.style.setProperty("--mx", (event.clientX - box.left) + "px");
        card.style.setProperty("--my", (event.clientY - box.top) + "px");
    });
}

/**
 * @brief applies the server's upload policy to the paperclip button; it stays visible when uploads are off, only dimmed, so a click can still explain why
 *
 * @return void
 */
function chat_files__apply_file_upload_policy_to_ui()
{
    if (typeof document === "undefined")
    {
        return;
    }

    let button = document.getElementById("custom-file-upload-button-file");

    if (button == null)
    {
        return;
    }

    if (g_server_policy.allow_file_uploads == true)
    {
        button.classList.remove("chat-file-uploads-off");
        button.title = "attach a file (up to " + chat_files__format_file_size(g_server_policy.file_upload_max_size) + ")";
    }
    else
    {
        button.classList.add("chat-file-uploads-off");
        button.title = "file uploads are not allowed on this server";
    }
}

/**
 * @brief the same treatment for the picture button: it stays visible when pictures are off, only dimmed
 *
 * @return void
 */
function chat_files__apply_chat_picture_policy_to_ui()
{
    if (typeof document === "undefined")
    {
        return;
    }

    let button = document.getElementById("custom-file-upload-button-image");

    if (button == null)
    {
        return;
    }

    if (g_server_policy.allow_chat_pictures == true)
    {
        button.classList.remove("chat-file-uploads-off");
        button.title = "attach a picture (up to " + chat_files__format_file_size(g_server_policy.chat_picture_max_size) + ")";
    }
    else
    {
        button.classList.add("chat-file-uploads-off");
        button.title = "inline pictures are disabled on this server";
    }
}

// ---------------------------------------------------------------------------
// server settings tab
// ---------------------------------------------------------------------------

/**
 * @brief shows the ram and upload-time warning while the typed limit is over FILE_UPLOAD_WARNING_MB
 *
 * @return void
 */
function chat_files__refresh_file_upload_size_warning()
{
    let input = document.getElementById("server-settings-general-file-upload-max-size-input");
    let warning = document.getElementById("server-settings-general-file-upload-size-warning");

    if (input == null || warning == null)
    {
        return;
    }

    let megabytes = parseInt(input.value);

    warning.style.display = (megabytes > FILE_UPLOAD_WARNING_MB) ? "block" : "none";
}

/**
 * @brief the pictures checkbox shows or hides the picture max-size row under it
 *
 * @return void
 */
function chat_files__refresh_picture_size_visibility()
{
    let checkbox = document.getElementById("server-settings-general-allow-pictures-checkbox");
    let size_row = document.getElementById("server-settings-picture-max-size-row");

    if (checkbox == null || size_row == null)
    {
        return;
    }

    size_row.style.display = (checkbox.checked == true) ? "" : "none";
}

/**
 * @brief the uploads checkbox shows or hides the max-size row below it; the stored limit stays server state either way
 *        the size warning only makes sense while the row is visible
 *
 * @return void
 */
function chat_files__refresh_file_upload_size_visibility()
{
    let checkbox = document.getElementById("server-settings-general-allow-file-uploads-checkbox");
    let size_row = document.getElementById("server-settings-file-upload-size-row");
    let warning = document.getElementById("server-settings-general-file-upload-size-warning");

    if (checkbox == null || size_row == null || warning == null)
    {
        return;
    }

    if (checkbox.checked == true)
    {
        size_row.style.display = "";
        chat_files__refresh_file_upload_size_warning();
    }
    else
    {
        size_row.style.display = "none";
        warning.style.display = "none";
    }
}
