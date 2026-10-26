// This file lets admins upload server emojis and loads their pictures when clients need them.

var server_emojis =
{
    entries: new Map(),
    images: new Map(),
    requested: new Set(),
    failed: new Set(),
    connected: false,
    generation: null,
    observer: null,
    page: 0,
    page_size: 24,
    upload_name: null,
    upload_sequence: 0,
    deleting: new Set(),
    confirmation: null,
    saving: false,
    status: ""
};

/**
 * @brief Connects the emoji settings controls and clears pictures after disconnection.
 */
function server_emojis__setup()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    document.getElementById("server-emojis-upload-form").onsubmit = server_emojis__upload;
    document.getElementById("server-emojis-search").oninput = function()
    {
        server_emojis.page = 0;
        server_emojis__render_admin();
    };
    document.getElementById("server-emojis-previous").onclick = function()
    {
        server_emojis.page--;
        server_emojis__render_admin();
    };
    document.getElementById("server-emojis-next").onclick = function()
    {
        server_emojis.page++;
        server_emojis__render_admin();
    };
    document.getElementById("server-emojis-refresh").onclick = function()
    {
        if (g_is_authenticated)
        {
            server_emojis.failed.clear();
            server_emojis.requested.clear();
            connection__send_message_object({ message: { type: "load_server_emojis" } });
        }
    };
    document.getElementById("server-emojis-save").onclick = function()
    {
        if (g_is_authenticated && g_is_local_client_admin && !server_emojis.saving)
        {
            server_emojis.saving = true;
            server_emojis.status = "Saving emojis…";
            connection__send_message_object({ message: { type: "save_server_settings" } });
            server_emojis__render_admin();
        }
    };
    g_connection_status_listeners.push(function()
    {
        if (!g_is_authenticated && server_emojis.connected)
        {
            server_emojis__reset();
        }
    });
    server_emojis.observer = new IntersectionObserver(function(entries)
    {
        for (let entry of entries)
        {
            if (entry.isIntersecting && entry.target.isConnected)
            {
                server_emojis.observer.unobserve(entry.target);
                server_emojis__request_image(entry.target.dataset.serverEmojiId);
            }
        }
    });
    server_emojis__render_admin();
}

/**
 * @brief Starts a fresh emoji list for the authenticated server connection.
 */
function server_emojis__connected()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    server_emojis__reset();
    server_emojis.connected = true;
    server_emojis.generation = g_worker_connection_generation;
    if (typeof g_loopback_port === "number" && g_loopback_port > 0)
    {
        // The background client may have received the list before this browser connected.
        connection__send_message_object({ message: { type: "load_server_emojis" } });
    }
}

/**
 * @brief Removes cached pictures and unfinished requests from the previous connection.
 */
function server_emojis__reset()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    server_emojis.entries.clear();
    server_emojis.images.clear();
    server_emojis.requested.clear();
    server_emojis.failed.clear();
    server_emojis.deleting.clear();
    server_emojis.connected = false;
    server_emojis.generation = null;
    server_emojis.upload_name = null;
    server_emojis.upload_sequence++;
    server_emojis.confirmation = null;
    server_emojis.saving = false;
    server_emojis.page = 0;
    server_emojis.status = "";
    if (server_emojis.observer)
    {
        server_emojis.observer.disconnect();
    }
    server_emojis__refresh_images();
    server_emojis__render_admin();
    server_emojis__notify_reactions();
}

/**
 * @brief Returns available server emojis in name order without downloading pictures.
 * @return Array -> emoji identifiers and names
 */
function server_emojis__get_entries()
{
    return Array.from(server_emojis.entries.values()).sort(function(first, second)
    {
        return first.name.localeCompare(second.name);
    });
}

/**
 * @brief Finds the current name of an emoji on this server.
 * @param id -> server emoji identifier
 * @return string -> current name or null when unavailable
 */
function server_emojis__name(id)
{
    let entry = server_emojis.entries.get(id);
    return entry ? entry.name : null;
}

/**
 * @brief Shows a cached emoji or its name and waits until it is visible before loading it.
 * @param element -> destination for the picture
 * @param id -> server emoji identifier
 * @param fallback_name -> name kept in the encrypted reaction
 */
function server_emojis__render_image(element, id, fallback_name)
{
    let entry = server_emojis.entries.get(id);
    let name = server_emojis__name(id) || (typeof fallback_name === "string" && /^[a-z0-9_]{1,32}$/.test(fallback_name) ? fallback_name : "deleted_emoji");
    let data = server_emojis.images.get(id);
    if (server_emojis.observer)
    {
        server_emojis.observer.unobserve(element);
    }
    element.classList.add("server-emoji-image");
    element.dataset.serverEmojiId = id;
    element.dataset.serverEmojiName = name;
    element.replaceChildren();
    element.classList.toggle("server-emoji-unicode", !!entry && !!entry.unicode);
    if (entry && entry.unicode)
    {
        element.textContent = entry.unicode;
    }
    else if (data)
    {
        let image = document.createElement("img");
        image.alt = ":" + name + ":";
        image.src = data;
        element.appendChild(image);
    }
    else
    {
        element.textContent = ":" + name + ":";
        if (server_emojis.entries.has(id) && server_emojis.observer)
        {
            server_emojis.observer.observe(element);
        }
    }
}

/**
 * @brief Stops watching emoji placeholders before their controls are removed.
 * @param container -> controls that are about to be removed
 */
function server_emojis__forget_images(container)
{
    if (!server_emojis.observer || !container)
    {
        return;
    }
    if (container.matches("[data-server-emoji-id]"))
    {
        server_emojis.observer.unobserve(container);
    }
    for (let element of container.querySelectorAll("[data-server-emoji-id]"))
    {
        server_emojis.observer.unobserve(element);
    }
}

/**
 * @brief Fetches a visible emoji once while its image is missing.
 * @param id -> server emoji identifier
 */
function server_emojis__request_image(id)
{
    if (!g_is_authenticated || !server_emojis.entries.has(id) || server_emojis.entries.get(id).unicode || server_emojis.images.has(id)
        || server_emojis.requested.has(id) || server_emojis.failed.has(id))
    {
        return;
    }
    server_emojis.requested.add(id);
    connection__send_message_object({ message: { type: "load_server_emoji", emoji_id: id } });
}

/**
 * @brief Replaces visible placeholders after an image arrives or an emoji is removed.
 * @param id -> one changed emoji, or null for every displayed emoji
 */
function server_emojis__refresh_images(id = null)
{
    if (!G_HAS_DOM)
    {
        return;
    }
    for (let element of document.querySelectorAll("[data-server-emoji-id]"))
    {
        if (id == null || element.dataset.serverEmojiId === id)
        {
            server_emojis__render_image(element, element.dataset.serverEmojiId, element.dataset.serverEmojiName);
        }
    }
}

/**
 * @brief Refreshes reaction choices after the server's emoji list changes.
 */
function server_emojis__notify_reactions()
{
    if (typeof chat_reactions__refresh_server_emojis === "function")
    {
        chat_reactions__refresh_server_emojis();
    }
}

/**
 * @brief Applies emoji lists, pictures and changes announced by the server.
 * @param message -> decrypted server message
 */
function server_emojis__receive(message)
{
    if (!G_HAS_DOM || !g_is_authenticated || !server_emojis.connected
        || server_emojis.generation !== g_worker_connection_generation)
    {
        return;
    }
    if (message.type === "server_settings_saved")
    {
        if (!server_emojis.saving)
        {
            return;
        }
        server_emojis.saving = false;
        server_emojis.status = message.success === true ? "Changes saved." : "Could not save the server settings.";
    }
    else if (message.type === "server_emoji_list")
    {
        let entries = new Map();
        for (let entry of Array.isArray(message.emojis) ? message.emojis.slice(0, 256) : [])
        {
            if (entry && typeof entry.id === "string" && typeof entry.name === "string"
                && /^[a-f0-9]{32}$/.test(entry.id) && /^[a-z0-9_]{1,32}$/.test(entry.name)
                && (entry.unicode == null || (typeof entry.unicode === "string" && entry.unicode.length > 0
                    && entry.unicode.length <= 32 && !/[\u0000-\u001f\u007f]/.test(entry.unicode))))
            {
                entries.set(entry.id, { id: entry.id, name: entry.name, unicode: entry.unicode || null });
            }
        }
        server_emojis.entries = entries;
        server_emojis.confirmation = null;
        for (let collection of [server_emojis.images, server_emojis.requested, server_emojis.failed])
        {
            for (let id of collection.keys())
            {
                if (!entries.has(id))
                {
                    collection.delete(id);
                }
            }
        }
        server_emojis__refresh_images();
        server_emojis__notify_reactions();
    }
    else if (message.type === "server_emoji_image")
    {
        let id = message.emoji_id;
        if (!server_emojis.entries.has(id) || !server_emojis.requested.has(id))
        {
            return;
        }
        server_emojis.requested.delete(id);
        if (typeof message.base64 !== "string" || message.base64.length > 349558
            || !/^data:image\/(png|jpeg|gif);base64,[A-Za-z0-9+/]+={0,2}$/.test(message.base64))
        {
            server_emojis.failed.add(id);
            return;
        }
        try
        {
            let bytes = atob(message.base64.slice(message.base64.indexOf(",") + 1));
            let gif = message.base64.startsWith("data:image/gif;");
            if (bytes.length > (gif ? 256 * 1024 : 30000))
            {
                server_emojis.failed.add(id);
                return;
            }
            if (gif)
            {
                server_emojis__check_gif(Uint8Array.from(bytes, function(value)
                {
                    return value.charCodeAt(0);
                }));
            }
        }
        catch (error)
        {
            server_emojis.failed.add(id);
            return;
        }
        server_emojis.images.set(id, message.base64);
        server_emojis__refresh_images(id);
        return;
    }
    else if (message.type === "server_emoji_added")
    {
        if (typeof message.id !== "string" || typeof message.name !== "string"
            || !/^[a-f0-9]{32}$/.test(message.id) || !/^[a-z0-9_]{1,32}$/.test(message.name)
            || (message.unicode != null && (typeof message.unicode !== "string" || !message.unicode.length
                || message.unicode.length > 32 || /[\u0000-\u001f\u007f]/.test(message.unicode)))
            || (server_emojis.entries.size >= 256 && !server_emojis.entries.has(message.id)))
        {
            return;
        }
        server_emojis.entries.set(message.id, { id: message.id, name: message.name, unicode: message.unicode || null });
        if (server_emojis.upload_name === message.name)
        {
            server_emojis.upload_name = null;
            document.getElementById("server-emojis-upload-form").reset();
            server_emojis.status = "Emoji uploaded. Save changes to keep it after restart.";
        }
        server_emojis__notify_reactions();
    }
    else if (message.type === "server_emoji_deleted")
    {
        let id = message.emoji_id;
        server_emojis.entries.delete(id);
        server_emojis.images.delete(id);
        server_emojis.requested.delete(id);
        server_emojis.failed.delete(id);
        if (server_emojis.confirmation === id)
        {
            server_emojis.confirmation = null;
        }
        if (server_emojis.deleting.delete(id))
        {
            server_emojis.status = "Emoji deleted. Save changes to keep this after restart.";
        }
        server_emojis__refresh_images(id);
        server_emojis__notify_reactions();
    }
    else if (message.type === "server_emoji_error")
    {
        if (message.operation === "upload_server_emoji")
        {
            server_emojis.upload_name = null;
        }
        if (message.emoji_id)
        {
            server_emojis.requested.delete(message.emoji_id);
            server_emojis.deleting.delete(message.emoji_id);
            server_emojis.failed.add(message.emoji_id);
        }
        server_emojis.status = typeof message.reason === "string" ? message.reason.slice(0, 256) : "The emoji request failed.";
    }
    server_emojis__render_admin();
}

/**
 * @brief Opens the admin's emoji page without downloading pictures from other pages.
 */
function server_emojis__open_admin()
{
    if (g_is_authenticated && g_is_local_client_admin)
    {
        server_emojis__render_admin();
    }
}

/**
 * @brief Shows one page of uploaded emojis and the controls available to the admin.
 */
function server_emojis__render_admin()
{
    if (!G_HAS_DOM || !document.getElementById("server-emojis-list"))
    {
        return;
    }
    let admin = g_is_authenticated && g_is_local_client_admin;
    let query = document.getElementById("server-emojis-search").value.trim().toLowerCase();
    let entries = server_emojis__get_entries().filter(function(entry)
    {
        return entry.name.includes(query);
    });
    let pages = Math.max(1, Math.ceil(entries.length / server_emojis.page_size));
    let list = document.getElementById("server-emojis-list");
    server_emojis.page = Math.max(0, Math.min(server_emojis.page, pages - 1));
    if (!admin)
    {
        server_emojis.confirmation = null;
        if (server_emojis.upload_name != null)
        {
            server_emojis.upload_sequence++;
        }
        server_emojis.upload_name = null;
        server_emojis.deleting.clear();
        server_emojis.saving = false;
    }
    document.getElementById("server-emojis-upload").disabled = !admin || server_emojis.upload_name != null;
    document.getElementById("server-emojis-save").disabled = !admin || server_emojis.saving
        || server_emojis.upload_name != null || server_emojis.deleting.size > 0;
    document.getElementById("server-emojis-previous").disabled = server_emojis.page === 0;
    document.getElementById("server-emojis-next").disabled = server_emojis.page >= pages - 1;
    document.getElementById("server-emojis-page").textContent = (server_emojis.page + 1) + " / " + pages;
    document.getElementById("server-emojis-status").textContent = server_emojis.status;
    server_emojis__forget_images(list);
    list.replaceChildren();
    for (let entry of entries.slice(server_emojis.page * server_emojis.page_size, (server_emojis.page + 1) * server_emojis.page_size))
    {
        let row = document.createElement("div");
        let image = document.createElement("span");
        let name = document.createElement("span");
        let remove = document.createElement("button");
        row.className = "server-emoji-row";
        name.textContent = ":" + entry.name + ":";
        server_emojis__render_image(image, entry.id, entry.name);
        remove.type = "button";
        remove.textContent = server_emojis.confirmation === entry.id ? "Confirm delete" : "Delete";
        remove.disabled = !admin || server_emojis.deleting.has(entry.id);
        remove.onclick = function()
        {
            if (!g_is_authenticated || !g_is_local_client_admin || server_emojis.entries.get(entry.id) !== entry)
            {
                return;
            }
            if (server_emojis.confirmation === entry.id)
            {
                server_emojis.confirmation = null;
                server_emojis.deleting.add(entry.id);
                connection__send_message_object({ message: { type: "delete_server_emoji", emoji_id: entry.id } });
            }
            else
            {
                server_emojis.confirmation = entry.id;
            }
            server_emojis__render_admin();
        };
        row.append(image, name, remove);
        if (server_emojis.confirmation === entry.id)
        {
            let cancel = document.createElement("button");
            cancel.type = "button";
            cancel.textContent = "Cancel";
            cancel.onclick = function()
            {
                server_emojis.confirmation = null;
                server_emojis__render_admin();
            };
            row.appendChild(cancel);
        }
        list.appendChild(row);
    }
    if (!entries.length)
    {
        list.textContent = query ? "No matching emojis." : "This server has no emojis.";
    }
}

/**
 * @brief Checks the GIF header and rejects images larger than the emoji dimensions.
 * @param bytes -> complete GIF file
 */
function server_emojis__check_gif(bytes)
{
    let signature = String.fromCharCode(...bytes.slice(0, 6));
    if (bytes.length < 14 || (signature !== "GIF87a" && signature !== "GIF89a"))
    {
        throw new Error("Choose a valid GIF image.");
    }
    let width = bytes[6] | (bytes[7] << 8);
    let height = bytes[8] | (bytes[9] << 8);
    if (!width || !height || width > 128 || height > 128)
    {
        throw new Error("GIF emojis must be at most 128 × 128 pixels.");
    }
}

/**
 * @brief Prepares an emoji in a separate thread and keeps GIF animation intact.
 * @param file -> selected image file
 * @return Promise -> image data URL within the server's emoji limits
 */
async function server_emojis__prepare_image(file)
{
    return image_upload__prepare(file, "emoji", 30000);
}

/**
 * @brief Sends a named emoji after checking the image and the current admin connection.
 * @param event -> submission of the upload form
 */
async function server_emojis__upload(event)
{
    event.preventDefault();
    if (!g_is_authenticated || !g_is_local_client_admin || server_emojis.upload_name != null)
    {
        return;
    }
    let name = document.getElementById("server-emojis-name").value.trim().toLowerCase();
    let file = document.getElementById("server-emojis-file").files[0];
    let generation = g_worker_connection_generation;
    if (!/^[a-z0-9_]{1,32}$/.test(name) || server_emojis__get_entries().some(function(entry)
    {
        return entry.name === name;
    }))
    {
        server_emojis.status = "Use a unique name with 1 to 32 lowercase letters, numbers or underscores.";
        server_emojis__render_admin();
        return;
    }
    server_emojis.upload_name = name;
    let sequence = ++server_emojis.upload_sequence;
    server_emojis.status = "Preparing emoji…";
    server_emojis__render_admin();
    try
    {
        let base64 = await server_emojis__prepare_image(file);
        if (!g_is_authenticated || !g_is_local_client_admin || generation !== g_worker_connection_generation
            || server_emojis.upload_sequence !== sequence || server_emojis.upload_name !== name)
        {
            return;
        }
        connection__send_message_object({ message: { type: "upload_server_emoji", name: name, base64: base64 } });
        server_emojis.status = "Uploading emoji…";
    }
    catch (error)
    {
        if (generation === g_worker_connection_generation && server_emojis.upload_sequence === sequence)
        {
            server_emojis.upload_name = null;
            server_emojis.status = error.message || "Could not read this image.";
        }
    }
    server_emojis__render_admin();
}
