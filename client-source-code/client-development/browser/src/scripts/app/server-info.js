// This file shows the server banner, occupied slots and uptime in the root
// channel description. It also shows public settings and lets admins set a banner.

var g_server_info =
{
    uptime_seconds: null,
    received_at: 0,
    used_slots: null,
    max_slots: null,
    visibility: { uptime: false, slots: false, banner: false, features: false },
    admin: { uptime_seconds: null, received_at: 0, used_slots: null, max_slots: null, last_memory_request: null },
    features: {},
    banner: "",
    banner_loaded: false,
    generation: 0,
    last_request: 0,
    slots_changed: false,
    timer: null,
    banner_timer: null
};

var SERVER_INFO_FEATURES =
[
    ["is_voice_chat_active", "Voice chat", "Allows users to talk to others in the same channel. Individual channels can still disable audio."],
    ["is_music_bot_audio_active", "Music bot audio", "Allows music bots to play audio to users in their channel."],
    ["is_video_streaming_allowed", "Video streaming", "Allows screen sharing and video-file streams within a channel. Individual channels can still disable video."],
    ["is_audio_over_websocket_allowed", "Audio/video over WebSocket", "Allows audio and video to travel between each client and the server over WebSocket, using TCP."],
    ["is_audio_over_webrtc_allowed", "WebRTC DataChannel (not peer-to-peer)"],
    ["is_private_messaging_allowed", "Private messages", "Allows users to send messages directly to one another, outside the channel chat."],
    ["is_file_upload_allowed", "File uploads", "Allows users to attach files to chat messages, within the server's size limit."],
    ["is_chat_picture_allowed", "Chat pictures", "Allows users to send pictures that appear inside the chat."],
    ["is_link_preview_allowed", "Link previews", "Lets the server look up shared links and show a preview, such as a title, description or image."],
    ["is_avatar_upload_allowed", "Avatars", "Allows users to upload a picture to represent their identity."],
    ["is_typing_indicator_allowed", "Typing indicators", "Shows when someone is typing a message, without sharing what they are writing."],
    ["is_client_rename_allowed", "Name changes", "Allows users to change their display name after connecting. Admins can still rename users when this is disabled."],
    ["is_temp_channel_creation_allowed", "Temporary channels", "Shows whether temporary channels are available. When server groups are enabled, group permissions decide who can create them. Each user can own one. It is removed when its creator changes channels or disconnects."],
    ["is_idle_mode_allowed", "Idle mode", "Lets users stay signed in while away, without receiving channel messages or audio/video."],
    ["is_fast_reconnect_allowed", "Fast reconnect", "Lets a client resume its previous session after a brief connection loss."],
    ["is_connection_check_enforced", "Disconnect on missed keep-alive messages", "Disconnects clients that stop sending Lemon Chat's keep-alive messages over their chat WebSocket."],
    ["is_identity_storage_enabled", "Saved identities", "Lets the server remember identities between visits and restore their saved groups ."],
    ["is_stored_client_list_allowed", "Offline user list", "Allows users to see saved identities even when those people are offline."],
    ["is_last_seen_enabled", "Last seen times", "Shows when a saved identity was last connected to this server."],
    ["is_offline_messaging_allowed", "Offline messages", "Holds private messages for users with permission who are offline and delivers them when they return. Waiting messages are lost if the server restarts."],
    ["is_display_country_flags_active", "Country flags", "Shows country flags based on users' connection addresses."],
    ["is_hide_clients_in_password_protected_channels_active", "Hide users in password-protected channels", "Hides which password-protected channel a user is in from people outside that channel."],
    ["is_sending_text_to_idle_clients_allowed", "Messages to idle users", "Allows private text messages to reach users who are connected in idle mode."],
    ["is_music_bot_marquee_visible_to_everyone", "Show music titles to everyone", "Shows a music bot's current song title even to users outside the bot's channel."]
];

/**
 * @brief Writes elapsed server time in days, hours, minutes and seconds.
 * @param number seconds -> elapsed seconds reported by the server
 * @return string -> readable uptime
 */
function server_info__format_uptime(seconds)
{
    let days = Math.floor(seconds / 86400);
    let hours = Math.floor(seconds / 3600) % 24;
    let minutes = Math.floor(seconds / 60) % 60;
    let remainder = Math.floor(seconds) % 60;
    let parts = [];

    if (days > 0)
    {
        parts.push(days + "d");
    }
    if (days > 0 || hours > 0)
    {
        parts.push(hours + "h");
    }
    parts.push(minutes + "m", remainder + "s");
    return parts.join(" ");
}

/**
 * @brief Checks whether a server information panel is visible.
 * @return boolean -> true while the root description or either settings view is open
 */
function server_info__is_visible()
{
    let elements = document.querySelectorAll("#root-server-info, #server-info-dialog");
    let element = null;

    for (element of elements)
    {
        if (element.getClientRects().length > 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Requests current server values, downloading the banner only when needed.
 * @param boolean include_banner -> whether the reply should include the image
 * @return void
 */
function server_info__request(include_banner = false)
{
    if (!g_is_authenticated || !groups__has_permission("view_server_info"))
    {
        return;
    }
    g_server_info.last_request = performance.now();
    g_server_info.slots_changed = false;
    connection__send_message_object({ message: { type: "load_server_info", include_banner: include_banner && g_server_info.visibility.banner } });
}

/**
 * @brief Requests admin settings and starts refreshing readings before the first reply arrives.
 * @param boolean memory_only -> true requests readings without replacing settings being edited
 * @return void
 */
function server_info__request_admin_settings(memory_only = false)
{
    if (!g_is_authenticated || !g_is_local_client_admin)
    {
        return;
    }
    g_server_info.admin.last_memory_request = performance.now();
    if (g_server_info.timer === null)
    {
        g_server_info.timer = setInterval(server_info__tick, 1000);
    }
    connection__send_message_object({ message: { type: "load_server_settings", memory_only: memory_only } });
}

/**
 * @brief Refreshes the visible slot count on the next timer tick after clients join or leave.
 * @return void
 */
function server_info__clients_changed()
{
    g_server_info.slots_changed = true;
}

/**
 * @brief Updates visible counters without sending a request every second.
 * @return void
 */
function server_info__tick()
{
    let ram_element = document.querySelector("[data-admin-server-ram]");

    server_info__render_counters(g_server_info, "[data-server-uptime]", "[data-server-slots]");
    server_info__render_counters(g_server_info.admin, "[data-admin-server-uptime]", "[data-admin-server-slots]");

    if (!document.hidden && g_is_authenticated && g_is_local_client_admin
        && g_server_info.admin.last_memory_request !== null && ram_element && ram_element.getClientRects().length > 0
        && performance.now() - g_server_info.admin.last_memory_request >= 5000)
    {
        server_info__request_admin_settings(true);
    }

    if (!document.hidden && server_info__is_visible()
        && (g_server_info.visibility.uptime || g_server_info.visibility.slots || g_server_info.visibility.features)
        && performance.now() - g_server_info.last_request >=
            (g_server_info.visibility.slots && g_server_info.slots_changed ? 1000 : 15000))
    {
        server_info__request(false);
    }
}

/**
 * @brief Fills counters from either the public reply or the separate admin reply.
 * @param object info -> received counters and their local arrival time
 * @param string uptime_selector -> uptime elements to update
 * @param string slots_selector -> slot elements to update
 * @return void
 */
function server_info__render_counters(info, uptime_selector, slots_selector)
{
    let uptime = "Unavailable";
    let slots = "Unavailable";
    let element = null;

    if (info.uptime_seconds !== null)
    {
        uptime = server_info__format_uptime(info.uptime_seconds
            + Math.max(0, performance.now() - info.received_at) / 1000);
    }
    if (info.used_slots !== null && info.max_slots !== null)
    {
        slots = info.used_slots + " / " + info.max_slots;
    }
    for (element of document.querySelectorAll(uptime_selector))
    {
        element.textContent = uptime;
    }
    for (element of document.querySelectorAll(slots_selector))
    {
        element.textContent = slots;
    }
}

/**
 * @brief Uses only the server's public fields to update the information views.
 * @param object data -> public server_info message body
 * @return void
 */
function server_info__apply(data)
{
    let field = null;
    let element = null;
    let banner = null;

    if (data.type === "server_info")
    {
        g_server_info.last_request = performance.now();
        for (field of ["uptime", "slots", "banner", "features"])
        {
            g_server_info.visibility[field] = data.visibility ? data.visibility[field] === true : true;
        }
        g_server_info.features = {};
    }

    if (g_server_info.visibility.uptime && typeof data.uptime_seconds === "number" && Number.isFinite(data.uptime_seconds) && data.uptime_seconds >= 0)
    {
        g_server_info.uptime_seconds = data.uptime_seconds;
        g_server_info.received_at = performance.now();
    }
    for (field of ["used_slots", "max_slots"])
    {
        if (g_server_info.visibility.slots && Number.isSafeInteger(data[field]) && data[field] >= 0)
        {
            g_server_info[field] = data[field];
        }
    }
    for (field of SERVER_INFO_FEATURES)
    {
        if (g_server_info.visibility.features && data.features && typeof data.features[field[0]] === "boolean")
        {
            g_server_info.features[field[0]] = data.features[field[0]];
        }
    }
    if (g_server_info.features.is_identity_storage_enabled === false)
    {
    }
    if (!g_server_info.visibility.uptime)
    {
        g_server_info.uptime_seconds = null;
    }
    if (!g_server_info.visibility.slots)
    {
        g_server_info.used_slots = null;
        g_server_info.max_slots = null;
    }
    if (!g_server_info.visibility.features)
    {
        g_server_info.features = {};
    }
    if (!g_server_info.visibility.banner)
    {
        g_server_info.banner = "";
        g_server_info.banner_loaded = false;
        clearTimeout(g_server_info.banner_timer);
        g_server_info.banner_timer = null;
    }
    else if (typeof data.server_banner === "string")
    {
        banner = data.server_banner;
        g_server_info.banner = server_info__validated_banner(banner);
        g_server_info.banner_loaded = true;
        clearTimeout(g_server_info.banner_timer);
        g_server_info.banner_timer = null;
    }
    else if (data.banner_changed === true)
    {
        g_server_info.banner_loaded = false;
        g_server_info.banner = "";
        // Spread image downloads out when many connected users receive a new banner.
        if (g_server_info.banner_timer === null)
        {
            g_server_info.banner_timer = setTimeout(function()
            {
                g_server_info.banner_timer = null;
                if (server_info__is_visible())
                {
                    server_info__request(true);
                }
            }, 500 + Math.random() * 4500);
        }
    }
    for (element of document.querySelectorAll("[data-server-banner]"))
    {
        element.hidden = g_server_info.banner === "";
        if (g_server_info.banner !== "" && element.getAttribute("src") !== g_server_info.banner)
        {
            element.src = g_server_info.banner;
        }
        else if (g_server_info.banner === "")
        {
            element.removeAttribute("src");
        }
    }
    if (g_server_info.timer === null && (g_server_info.visibility.uptime || g_server_info.visibility.slots
        || g_server_info.visibility.features || g_server_info.admin.last_memory_request !== null))
    {
        g_server_info.timer = setInterval(server_info__tick, 1000);
    }
    else if (!g_server_info.visibility.uptime && !g_server_info.visibility.slots
        && !g_server_info.visibility.features && g_server_info.admin.last_memory_request === null)
    {
        clearInterval(g_server_info.timer);
        g_server_info.timer = null;
    }
    server_info__apply_visibility();
    server_info__render_features();
    server_info__tick();
}

/**
 * @brief Accepts small embedded banner images without allowing external image requests.
 * @param string banner -> image data URL from the server
 * @return string -> safe image URL, or an empty string
 */
function server_info__validated_banner(banner)
{
    return typeof banner === "string" && banner.length <= 44000
        && /^data:image\/(png|jpeg|webp);base64,[A-Za-z0-9+/]+={0,2}$/.test(banner) ? banner : "";
}

/**
 * @brief Removes information from the page as soon as the server stops sharing it.
 * @return void
 */
function server_info__apply_visibility()
{
    let element = null;
    let root = document.getElementById("root-server-info");
    let dialog = document.getElementById("server-info-dialog");

    for (element of document.querySelectorAll("[data-server-info-field]"))
    {
        element.hidden = !g_server_info.visibility[element.dataset.serverInfoField];
    }
    for (element of document.querySelectorAll(".context-menu-item[data-action='server-info']"))
    {
        element.hidden = !g_server_info.visibility.features;
    }
    if (root)
    {
        root.hidden = !g_server_info.visibility.uptime && !g_server_info.visibility.slots && !g_server_info.visibility.banner;
    }
    if (dialog && dialog.open && !g_server_info.visibility.features)
    {
        dialog.close();
    }
}

/**
 * @brief Fills the private admin preview without copying its values into the public view.
 * @param object data -> administrator's server_settings_values reply
 * @return void
 */
function server_info__apply_admin_settings(data)
{
    let field = null;
    let image = document.querySelector("[data-admin-server-banner]");
    let banner = server_info__validated_banner(data.server_banner);

    for (field of ["uptime_seconds", "used_slots", "max_slots"])
    {
        g_server_info.admin[field] = Number.isSafeInteger(data[field]) && data[field] >= 0 ? data[field] : null;
    }
    g_server_info.admin.received_at = performance.now();
    server_info__apply_admin_memory(data);
    image.hidden = banner === "";
    if (banner !== "")
    {
        image.src = banner;
    }
    else
    {
        image.removeAttribute("src");
    }
    if (g_server_info.timer === null)
    {
        g_server_info.timer = setInterval(server_info__tick, 1000);
    }
    server_info__tick();
}

/**
 * @brief Updates the admin's RAM, slots and uptime without changing settings inputs.
 * @param object data -> server reply with the current readings
 * @return void
 */
function server_info__apply_admin_memory(data)
{
    let element = document.querySelector("[data-admin-server-ram]");
    let field = null;
    let websocket = 0;
    let webrtc = 0;
    let total = null;
    let unit = 0;
    let units = ["B", "KiB", "MiB", "GiB", "TiB"];
    let has_ram_reading = Object.prototype.hasOwnProperty.call(data, "ram_bytes");

    for (field of ["uptime_seconds", "used_slots", "max_slots"])
    {
        if (Number.isSafeInteger(data[field]) && data[field] >= 0)
        {
            g_server_info.admin[field] = data[field];
        }
    }
    if (Number.isSafeInteger(data.uptime_seconds) && data.uptime_seconds >= 0)
    {
        g_server_info.admin.received_at = performance.now();
    }
    server_info__render_counters(g_server_info.admin, "[data-admin-server-uptime]", "[data-admin-server-slots]");
    g_server_info.admin.last_memory_request = performance.now();
    if (element)
    {
        element.textContent = Number.isSafeInteger(data.ram_bytes) && data.ram_bytes >= 0
            ? (data.ram_bytes / (1024 * 1024)).toLocaleString(undefined, { maximumFractionDigits: 1 }) + " MiB"
            : has_ram_reading ? "The server could not read its RAM usage."
                : "The server did not send a RAM reading.";
    }
    for (field of ["received", "sent"])
    {
        element = document.querySelector("[data-admin-server-" + field + "]");
        websocket = data["websocket_" + field + "_bytes"];
        webrtc = data["webrtc_" + field + "_bytes"];
        if (!element)
        {
            continue;
        }
        if (!Number.isSafeInteger(websocket) || websocket < 0 || !Number.isSafeInteger(webrtc) || webrtc < 0)
        {
            element.textContent = "The server did not send a traffic reading.";
            element.removeAttribute("title");
            continue;
        }
        total = websocket + webrtc;
        unit = 0;
        while (total >= 1024 && unit < units.length - 1)
        {
            total /= 1024;
            unit++;
        }
        element.textContent = total.toLocaleString(undefined, {maximumFractionDigits: 1}) + " " + units[unit];
        element.title = "WebSocket: " + websocket.toLocaleString() + " bytes; WebRTC: " + webrtc.toLocaleString() + " bytes";
    }
}

/**
 * @brief Shows the channel description, adding server details only for the root.
 * @param object channel -> selected channel from the current channel list
 * @return void
 */
function server_info__show_channel_description(channel)
{
    let container = document.getElementById("current-channel-description");
    let description = document.createElement("div");

    container.replaceChildren();
    container.removeAttribute("data-description-channel-id");
    if (!channel)
    {
        return;
    }
    container.dataset.descriptionChannelId = channel.channel_id;
    if (channel.is_root_channel === true)
    {
        container.innerHTML = "<section id='root-server-info' class='server-info-summary'>"
            + "<img data-server-banner class='server-info-banner' alt='Server banner' hidden>"
            + "<dl><div data-server-info-field='slots'><dt>Used slots</dt><dd data-server-slots>Unavailable</dd></div>"
            + "<div data-server-info-field='uptime'><dt>Uptime</dt><dd data-server-uptime>Unavailable</dd></div></dl></section>";
        server_info__apply({});
        server_info__request(!g_server_info.banner_loaded);
    }
    description.className = "channel-description-text";
    description.textContent = channel.description || "";
    container.appendChild(description);
}

/**
 * @brief Lists the settings that the server shares with connected users.
 * @return void
 */
function server_info__render_features()
{
    let container = document.getElementById("server-info-features");
    let field = null;
    let row = null;
    let label = null;
    let value = null;

    if (!container)
    {
        return;
    }
    container.replaceChildren();
    for (field of SERVER_INFO_FEATURES)
    {
        if (typeof g_server_info.features[field[0]] !== "boolean")
        {
            continue;
        }
        row = document.createElement("div");
        label = document.createElement("dt");
        value = document.createElement("dd");
        label.textContent = field[1];
        label.title = field[2] || "";
        label.className = "server-info-help-label";
        label.tabIndex = 0;
        value.textContent = g_server_info.features[field[0]] ? "Enabled" : "Disabled";
        row.append(label, value);
        if (field[0] === "is_audio_over_webrtc_allowed")
        {
            label.title = document.getElementById("local-settings-webrtc-note").textContent;
            label.setAttribute("aria-describedby", "local-settings-webrtc-note");
        }
        row.title = label.title;
        row.className = "server-info-help";
        container.appendChild(row);
    }
    if (container.childElementCount === 0)
    {
        container.textContent = "Server settings are not available yet.";
    }
}

/**
 * @brief Opens the public server settings window without joining another channel.
 * @return void
 */
function server_info__open()
{
    let dialog = document.getElementById("server-info-dialog");

    if (!g_server_info.visibility.features)
    {
        return;
    }
    server_info__render_features();
    server_info__tick();
    if (!dialog.open)
    {
        dialog.style.position = "";
        dialog.style.margin = "";
        dialog.style.left = "";
        dialog.style.top = "";
        dialog.style.right = "";
        dialog.style.bottom = "";
        dialog.showModal();
    }
    server_info__request(false);
}

/**
 * @brief Uploads a small banner after resizing the selected image to fit the panel.
 * @param Event event -> file input change event
 * @return Promise -> completes when the image has been prepared and sent
 */
async function server_info__upload_banner(event)
{
    let file = event.target.files[0];
    let image = null;
    let url = null;
    let canvas = null;
    let scale = 1;
    let quality = 0.85;
    let banner = "";
    let status = document.getElementById("server-banner-status");
    let generation = ++g_server_info.generation;

    event.target.value = "";
    if (!file)
    {
        return;
    }
    if (!["image/png", "image/jpeg", "image/webp"].includes(file.type) || file.size > 10 * 1024 * 1024)
    {
        status.textContent = "Choose a PNG, JPEG or WebP image up to 10 MB.";
        return;
    }
    status.textContent = "Preparing banner…";
    try
    {
        url = URL.createObjectURL(file);
        image = new Image();
        image.src = url;
        await image.decode();
        scale = Math.min(1, 1024 / image.naturalWidth, 256 / image.naturalHeight);
        canvas = document.createElement("canvas");
        canvas.width = Math.max(1, Math.round(image.naturalWidth * scale));
        canvas.height = Math.max(1, Math.round(image.naturalHeight * scale));
        canvas.getContext("2d").drawImage(image, 0, 0, canvas.width, canvas.height);
        do
        {
            banner = canvas.toDataURL("image/webp", quality);
            quality -= 0.1;
        }
        while (banner.length > 32768 && quality >= 0.15);
        if (banner.length > 43716)
        {
            status.textContent = "This image is too detailed. Choose a smaller or simpler banner.";
            return;
        }
        if (!g_is_authenticated || !channel_tree__is_local_client_admin() || generation !== g_server_info.generation)
        {
            status.textContent = "Connect as an admin to change the banner.";
            return;
        }
        connection__send_message_object({ message: { type: "set_server_banner", banner: banner } });
        status.textContent = "Saving banner…";
    }
    catch (error)
    {
        status.textContent = "Could not read this image.";
    }
    finally
    {
        if (url !== null)
        {
            URL.revokeObjectURL(url);
        }
    }
}

/**
 * @brief Removes the server banner using the same admin request as an upload.
 * @return void
 */
function server_info__remove_banner()
{
    g_server_info.generation++;
    connection__send_message_object({ message: { type: "set_server_banner", banner: "" } });
    document.getElementById("server-banner-status").textContent = "Removing banner…";
}

/**
 * @brief Reports whether the server saved the admin's banner change.
 * @param object data -> server_banner_result reply
 * @return void
 */
function server_info__banner_result(data)
{
    document.getElementById("server-banner-status").textContent = data.success === true
        ? "Banner saved." : data.error === "server_ram_budget_reached"
            ? "The server has reached its RAM budget. Try uploading again later."
            : "Could not save the banner. Check the image size and your admin access.";
    if (data.success === true)
    {
        server_info__request_admin_settings();
        if (g_server_info.visibility.banner)
        {
            server_info__request(true);
        }
    }
}

/**
 * @brief Clears values and timers when leaving a server so they cannot follow a new connection.
 * @return void
 */
function server_info__reset()
{
    let dialog = document.getElementById("server-info-dialog");
    let banner = document.querySelector("[data-admin-server-banner]");
    let element = null;

    clearInterval(g_server_info.timer);
    clearTimeout(g_server_info.banner_timer);
    g_server_info.timer = null;
    g_server_info.generation++;
    g_server_info.banner_timer = null;
    g_server_info.uptime_seconds = null;
    g_server_info.used_slots = null;
    g_server_info.max_slots = null;
    g_server_info.visibility = { uptime: false, slots: false, banner: false, features: false };
    g_server_info.admin = { uptime_seconds: null, received_at: 0, used_slots: null, max_slots: null, last_memory_request: null };
    g_server_info.features = {};
    g_server_info.banner = "";
    g_server_info.banner_loaded = false;
    g_server_info.last_request = 0;
    g_server_info.slots_changed = false;
    document.getElementById("server-banner-status").textContent = "";
    server_info__render_counters(g_server_info.admin, "[data-admin-server-uptime]", "[data-admin-server-slots]");
    if (banner)
    {
        banner.hidden = true;
        banner.removeAttribute("src");
    }
    for (element of document.querySelectorAll("[data-admin-server-ram], [data-admin-server-received], [data-admin-server-sent]"))
    {
        element.textContent = "Waiting for server readings…";
        element.removeAttribute("title");
    }
    if (dialog && dialog.open)
    {
        dialog.close();
    }
    server_info__apply({});
}
