// This file provides controls for delegated moderation and channel permissions.
var moderation =
{
    channels: new Map(),
    clients: new Map(),
    pending_join: null,
    dialog: null,
    body: null,
    status: null,
    emoji_parent: null,
    channel_edit: null
};

/**
 * @brief Creates a control using the settings window button styles.
 * @param label Visible action name.
 * @param action Click handler.
 */
function moderation__button(label, action)
{
    let button = document.createElement("button");
    button.type = "button";
    button.textContent = label;
    button.onclick = action;
    return button;
}

/**
 * @brief Opens a nonblocking dialog and restores the emoji panel on close.
 * @param title Window title.
 */
function moderation__open(title, modal = true)
{
    if (!moderation.dialog)
    {
        let dialog = document.createElement("dialog");
        let heading = document.createElement("header");
        let label = document.createElement("strong");
        let close = moderation__button("", function()
        {
            dialog.close();
        });
        dialog.id = "moderation-dialog";
        label.id = "moderation-title";
        dialog.setAttribute("aria-labelledby", label.id);
        heading.className = "groups-titlebar";
        close.className = "close-button";
        close.setAttribute("aria-label", "Close");
        heading.append(label, close);
        moderation.body = document.createElement("div");
        moderation.body.className = "moderation-content";
        moderation.status = document.createElement("p");
        moderation.status.setAttribute("role", "status");
        dialog.append(heading, moderation.body, moderation.status);
        dialog.onclose = function()
        {
            if (dialog.open)
            {
                return;
            }
            moderation__restore_emojis();
            moderation.body.replaceChildren();
        };
        document.body.append(dialog);
        moderation.dialog = dialog;
    }
    if (moderation.dialog.open && moderation.dialog.matches(":modal") !== modal)
    {
        moderation.dialog.close();
    }
    moderation.dialog.dataset.modeless = String(!modal);
    moderation__restore_emojis();
    moderation.body.replaceChildren();
    moderation.status.textContent = "";
    document.getElementById("moderation-title").textContent = title;
    if (!moderation.dialog.open)
    {
        if (modal)
        {
            moderation.dialog.showModal();
        }
        else
        {
            moderation.dialog.show();
        }
    }
    return moderation.body;
}

/**
 * @brief Returns the shared emoji controls to server settings.
 */
function moderation__restore_emojis()
{
    if (moderation.emoji_parent)
    {
        moderation.emoji_parent.append(document.getElementById("server-emojis-panel"));
        moderation.emoji_parent = null;
    }
}

/**
 * @brief Sets up moderation state and client dragging for this connection.
 */
function moderation__setup()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    moderation__setup_drag();
    g_connection_status_listeners.push(function()
    {
        if (!g_is_authenticated)
        {
            moderation.channels.clear();
            moderation.clients.clear();
            moderation.pending_join = null;
            moderation.channel_edit = null;
            if (moderation.dialog && moderation.dialog.open)
            {
                moderation.dialog.close();
            }
        }
        moderation__refresh();
    });
    moderation__refresh();
}

/**
 * @brief Refreshes controls after the server changes the user's permissions.
 */
function moderation__refresh()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    if (typeof channel_groups__render_member_controls === "function")
    {
        channel_groups__render_member_controls();
    }
    moderation__refresh_menu_access();
    for (let menu of document.querySelectorAll("#contextmenus-container [data-moderation-client]"))
    {
        moderation__render_client_menu(menu);
        let target = moderation__target(menu.dataset.moderationClient, menu.dataset.identity, menu.dataset.channelId);
        moderation__request_client_access(target);
    }
    for (let menu of document.querySelectorAll("#contextmenus-container [data-context-channel]"))
    {
        moderation__send("channel_access_request", { channel_id: Number(menu.dataset.contextChannel) });
    }
    let ip = document.getElementById("client-info-ip");
    if (ip && !groups__has_permission("view_ip_addresses"))
    {
        ip.textContent = "Not permitted";
    }
    if (typeof server_emojis__render_admin === "function")
    {
        server_emojis__render_admin();
    }
    if (moderation.dialog && moderation.dialog.open && moderation.dialog.dataset.page === "bans"
        && !groups__has_permission("view_bans"))
    {
        moderation.dialog.close();
    }
}

/**
 * @brief Opens management features without requesting full server settings.
 */
function moderation__tools()
{
    if (!moderation__menu_action_allowed("server-moderation"))
    {
        return;
    }
    let body = moderation__open("Server moderation");
    moderation.dialog.dataset.page = "tools";
    if (groups__has_permission("view_bans"))
    {
        body.append(moderation__button("Bans", function()
        {
            moderation__open("Bans");
            moderation.dialog.dataset.page = "bans";
            moderation__send("manage_bans");
        }));
    }
    if (["upload_emojis", "delete_emojis"].some(groups__has_permission))
    {
        body.append(moderation__button("Emojis", function()
        {
            let content = moderation__open("Server emojis");
            moderation.dialog.dataset.page = "emojis";
            let panel = document.getElementById("server-emojis-panel");
            moderation.emoji_parent = panel.parentElement;
            content.append(panel);
            server_emojis__render_admin();
        }));
    }
    if (g_server_policy.is_permission_system_enabled && ["upload_icons", "delete_icons"].some(groups__has_permission))
    {
        body.append(moderation__button("Server icons", function()
        {
            moderation.dialog.close();
            groups_window.selected = g_groups.length ? g_groups[0].group_id : null;
            groups__open_icon_picker();
        }));
    }
}

/**
 * @brief Sends an ordinary authenticated server request.
 * @param type Message type.
 * @param fields Additional request fields.
 */
function moderation__send(type, fields = {})
{
    connection__send_message_object({ message: Object.assign({ type: type }, fields) });
}

/**
 * @brief Checks the server's latest rights for a channel.
 * @param channel_id Channel being changed.
 * @param permission Required action.
 */
function moderation__allows(channel_id, permission)
{
    if (g_is_local_client_admin)
    {
        return true;
    }
    let access = moderation.channels.get(Number(channel_id));
    let local = channel_tree__get_client_by_client_id(g_local_client_id);
    if (access && access.rights && Object.prototype.hasOwnProperty.call(access.rights, permission))
    {
        return access.rights[permission] === true;
    }
    if (local && !local.is_idle && Number(local.channel_id) === Number(channel_id) && channel_groups__active(local))
    {
        return false;
    }
    return groups__has_permission(permission);
}

/**
 * @brief Builds a select control with safely rendered labels.
 * @param label Accessible control name.
 * @param choices Pairs of values and visible names.
 */
function moderation__select(label, choices)
{
    let select = document.createElement("select");
    select.setAttribute("aria-label", label);
    for (let [value, name] of choices)
    {
        let option = document.createElement("option");
        option.value = value;
        option.textContent = name;
        select.append(option);
    }
    return select;
}

/** Resolves the selected identity again, since a menu can outlive a channel move. */
function moderation__target(client_id, identity, channel_id)
{
    let target = channel_tree__get_client_by_client_id(Number(client_id));
    return g_is_authenticated && target && !target.is_idle && !target.is_music_bot
        && target.public_key === identity && Number(target.channel_id) === Number(channel_id) ? target : null;
}

function moderation__request_client_access(target)
{
    if (target && !target.is_idle && !target.is_music_bot)
    {
        moderation__send("channel_access_request",
        {
            channel_id: Number(target.channel_id), client_id: target.client_id, identity: target.public_key
        });
    }
}

function moderation__client_access(target)
{
    let access = target && moderation.clients.get(Number(target.client_id));
    return access && access.identity === target.public_key && Number(access.channel_id) === Number(target.channel_id) ? access : null;
}

function moderation__can_act(target, action)
{
    if (!target || target.is_idle || target.is_music_bot || Number(target.client_id) === Number(g_local_client_id))
    {
        return false;
    }
    if (action === "kick_from_channel" && Number(target.channel_id) === 0)
    {
        return false;
    }
    let access = moderation__client_access(target);
    return access ? access.actions?.[action] === true : g_is_local_client_admin;
}

function moderation__can_assign_role(target, role)
{
    let access = moderation__client_access(target);
    return !!target && (access ? access.channel_groups.includes(Number(role)) : g_is_local_client_admin);
}

/**
 * @brief Confirms kick or ban against a captured identity before sending a request.
 * @param target Selected client, captured before the context menu closes.
 * @param action Server kick, server ban, or kick_from_channel.
 */
function moderation__confirm_client_action(target, action)
{
    if (!G_HAS_DOM || !g_is_authenticated || !target || target.is_music_bot
        || Number(target.client_id) === Number(g_local_client_id)
        || !["kick", "ban", "kick_from_channel"].includes(action))
    {
        return;
    }
    let client_id = Number(target.client_id);
    let identity = target.public_key;
    let channel_id = Number(target.channel_id);
    let generation = g_worker_connection_generation;
    let channel_action = action === "kick_from_channel";
    function current_target()
    {
        if (!g_is_authenticated || generation !== g_worker_connection_generation)
        {
            return null;
        }
        let current = channel_action ? moderation__target(client_id, identity, channel_id)
            : channel_tree__get_client_by_client_id(client_id);
        if (!current || current.public_key !== identity || current.is_music_bot
            || Number(current.client_id) === Number(g_local_client_id))
        {
            return null;
        }
        let allowed = channel_action ? channel_id !== 0 && moderation__can_act(current, action)
            : groups__can_moderate(current, action);
        return allowed ? current : null;
    }
    if (!current_target())
    {
        return;
    }
    let label = action === "ban" ? "Ban from server" : channel_action ? "Kick from channel" : "Kick from server";
    let question = document.createElement("p");
    let explanation = document.createElement("p");
    let actions = document.createElement("div");
    let spacer = document.createElement("span");
    let body = moderation__open(label);
    moderation.dialog.dataset.page = "confirmation";
    question.id = "moderation-confirm-question";
    question.textContent = label + ": “" + target.username + "”?";
    explanation.textContent = action === "ban"
        ? "They will be disconnected and blocked from reconnecting until the ban is removed."
        : channel_action ? "They will leave this channel and remain connected to the server."
            : "They will be disconnected, but can connect again.";
    let cancel = moderation__button("Cancel", function()
    {
        moderation.dialog.close();
    });
    let accept = moderation__button(label, function()
    {
        if (!moderation.dialog.open || !body.contains(accept))
        {
            return;
        }
        let current = current_target();
        if (!current)
        {
            accept.disabled = true;
            moderation.status.textContent = "The user or your permissions changed. Close this dialog and select the action again.";
            cancel.focus();
            return;
        }
        accept.disabled = true;
        moderation.dialog.close();
        if (channel_action)
        {
            moderation__send("moderate_client",
            {
                client_id: client_id, identity: identity, channel_id: channel_id, action: action
            });
        }
        else
        {
            moderation__send(action, { client_id: client_id });
        }
    });
    actions.append(spacer, cancel, accept);
    body.append(question, explanation, actions);
    UI.delete_contextmenus(true);
    cancel.focus();
}

function moderation__act(target, action, extra = {})
{
    if (!target || !moderation__can_act(target, action))
    {
        return;
    }
    if (action === "kick_from_channel")
    {
        moderation__confirm_client_action(target, action);
        return;
    }
    moderation__send("moderate_client", Object.assign(
    {
        client_id: target.client_id, identity: target.public_key, channel_id: Number(target.channel_id), action: action
    }, extra));
    UI.delete_contextmenus(true);
}

/** Keeps restricted actions discoverable without permitting activation. */
function moderation__set_menu_access(item, allowed, reason = "Your permissions do not allow this action.")
{
    item.hidden = false;
    item.classList.toggle("context-menu-item-disabled", !allowed);
    item.setAttribute("aria-disabled", String(!allowed));
    item.setAttribute("role", "menuitem");
    item.tabIndex = allowed ? 0 : -1;
    item.title = allowed ? "" : reason;
}

/** Rechecks ordinary menu actions both when rendered and when clicked. */
function moderation__menu_action_allowed(action, menu)
{
    let channel_id = Number(menu?.dataset.contextChannel ?? g_selected_channel_id);
    let target = channel_tree__get_client_by_client_id(Number(menu?.dataset.contextClient ?? g_selected_client_id));
    switch (String(action))
    {
        case "0":
            return groups__has_permission("create_channel") || groups__has_permission("create_temp_channel");
        case "1":
            return moderation__allows(channel_id, "delete_channel");
        case "2":
            return ["edit_channel_name", "edit_channel_description", "edit_channel_password",
                "edit_channel_powers", "edit_channel_options"].some(function(permission)
            {
                return moderation__allows(channel_id, permission);
            });
        case "11":
            return groups__has_permission("call_idle_clients");
        case "12":
        case "13":
            return groups__can_moderate(target, String(action) === "12" ? "kick" : "ban");
        case "14":
            return groups__has_permission("create_music_bots");
        case "15":
            return ["upload_music_tracks", "delete_music_tracks", "control_music_playback"].some(groups__has_permission);
        case "16":
            return groups__has_permission("delete_music_bots");
        case "server-moderation":
            return ["view_bans", "upload_emojis", "delete_emojis", "upload_icons", "delete_icons"].some(groups__has_permission);
        case "server-info":
            return groups__has_permission("view_server_info") && g_server_info.visibility.features === true;
        default:
            return true;
    }
}

function moderation__refresh_menu_access()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    for (let menu of document.querySelectorAll("#contextmenus-container .channel-list-context-menu"))
    {
        for (let item of menu.querySelectorAll(".context-menu-item[data-action]"))
        {
            if (item.dataset.action === "server-moderation" && !item.querySelector("svg"))
            {
                item.prepend(moderation__menu_icon("server_moderation"));
            }
            let reason = item.dataset.action === "server-info" && groups__has_permission("view_server_info")
                ? "The server is not sharing this information." : "Your permissions do not allow this action.";
            moderation__set_menu_access(item, moderation__menu_action_allowed(item.dataset.action, menu), reason);
            if (!item.dataset.menuKeyboard)
            {
                item.dataset.menuKeyboard = "true";
                item.addEventListener("keydown", function(event)
                {
                    if (event.key === "Enter" || event.key === " ")
                    {
                        event.preventDefault();
                        event.stopPropagation();
                        if (item.getAttribute("aria-disabled") !== "true")
                        {
                            item.click();
                        }
                    }
                });
            }
        }
    }
}

/** Uses the same icon slot and theme visibility as group assignment actions. */
function moderation__menu_icon(action)
{
    let paths =
    {
        move: "M2 4h9v12H2zM6 10h12m-4-4 4 4-4 4",
        mute: "M8 3a2 2 0 0 1 4 0v6a2 2 0 0 1-4 0V3ZM5 8v1a5 5 0 0 0 8 4M10 14v4m-3 0h6M2 2l16 16",
        kick_from_channel: "M2 3h7v14H2M6 10h12m-4-4 4 4-4 4",
        stop_stream: "M3 3h14v11H3zM6 18h8m-4-4v4M8 6h4v5H8z",
        server_moderation: "M3 5h14M3 10h14M3 15h14M6 3v4m8 1v4m-7 1v4"
    };
    let icon = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    icon.setAttribute("viewBox", "0 0 20 20");
    icon.setAttribute("class", "group-assignment-parent-icon");
    icon.setAttribute("aria-hidden", "true");
    let path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", paths[action]);
    icon.append(path);
    return icon;
}

function moderation__attach_client_menu(menu, target)
{
    if (!menu || !target || target.is_music_bot)
    {
        return;
    }
    menu.dataset.moderationClient = target.client_id;
    menu.dataset.identity = target.public_key;
    menu.dataset.channelId = target.channel_id;
    let local_mute = menu.querySelector('[data-action="5"]');
    if (local_mute)
    {
        local_mute.textContent = target.is_muted_by_local_client ? "Unmute locally" : "Mute locally";
    }
    moderation.clients.delete(Number(target.client_id));
    moderation__render_client_menu(menu);
    if (!g_server_policy.is_permission_system_enabled)
    {
        moderation__request_client_access(target);
    }
}

function moderation__render_client_menu(menu)
{
    menu.querySelectorAll(".client-moderation-action").forEach(function(item)
    {
        if (item.getAttribute("aria-expanded") === "true")
        {
            groups__close_assignment_menu();
        }
        item.remove();
    });
    let target = moderation__target(menu.dataset.moderationClient, menu.dataset.identity, menu.dataset.channelId);
    if (!target || Number(target.client_id) === Number(g_local_client_id))
    {
        return;
    }
    let items = menu.querySelector(".channel-list-context-menu-items");
    let before = items.querySelector('[data-action="12"], [data-action="13"]');
    for (let [action, label, submenu] of [["move", "Move to channel", true], ["mute", "Mute for everyone", true],
        ["kick_from_channel", "Kick from channel", false], ["stop_stream", "Stop stream", false]])
    {
        let item = document.createElement("p");
        item.className = "context-menu-item client-moderation-action" + (submenu ? " group-assignment-parent" : "");
        item.append(moderation__menu_icon(action), document.createTextNode(label));
        item.dataset.moderationAction = action;
        moderation__set_menu_access(item, moderation__can_act(target, action),
            action === "kick_from_channel" && Number(target.channel_id) === 0 ? "This user is already in root." : undefined);
        if (submenu)
        {
            item.setAttribute("aria-haspopup", "menu");
            item.setAttribute("aria-expanded", "false");
        }
        function activate(event)
        {
            event.stopPropagation();
            let current = moderation__target(menu.dataset.moderationClient, menu.dataset.identity, menu.dataset.channelId);
            if (!moderation__can_act(current, action))
            {
                groups__close_assignment_menu();
                return;
            }
            if (submenu)
            {
                moderation__open_action_menu(item, current, action);
            }
            else
            {
                moderation__act(current, action);
            }
        }
        item.addEventListener("click", activate);
        item.addEventListener("mouseenter", function(event)
        {
            if (submenu)
            {
                activate(event);
            }
            else
            {
                groups__close_assignment_menu();
            }
        });
        item.addEventListener("keydown", function(event)
        {
            if (["Enter", " ", "ArrowRight"].includes(event.key))
            {
                event.preventDefault();
                activate(event);
                document.querySelector('#group-assignment-menu .group-assignment-option[aria-disabled="false"]')?.focus();
            }
        });
        items.insertBefore(item, before);
    }
    let box = menu.getBoundingClientRect();
    menu.style.top = Math.max(8, Math.min(box.top, window.innerHeight - box.height - 8)) + "px";
}

function moderation__open_action_menu(item, target, action)
{
    groups__close_assignment_menu();
    if (!moderation__can_act(target, action))
    {
        return;
    }
    let menu = groups__create_submenu(item);
    let items = menu.querySelector(".channel-list-context-menu-items");
    let choices = action === "move" ? g_channel_list.filter(function(channel)
    {
        return Number(channel.channel_id) !== Number(target.channel_id);
    }).map(function(channel)
    {
        return [Number(channel.channel_id), channel.name || channel.channel_name || String(channel.channel_id)];
    }) : [[60, "Mute in channel · 1 minute"], [300, "Mute in channel · 5 minutes"],
        [1800, "Mute in channel · 30 minutes"], [3600, "Mute in channel · 1 hour"], [0, "Remove channel mute"]];
    for (let [value, label] of choices)
    {
        let button = document.createElement("p");
        button.textContent = label;
        button.addEventListener("click", function(event)
        {
            event.stopPropagation();
            let current = moderation__target(target.client_id, target.public_key, target.channel_id);
            moderation__act(current, action, action === "move" ? { destination_channel_id: value } : { duration_seconds: value });
        });
        button.className = "context-menu-item group-assignment-option";
        moderation__set_menu_access(button, true);
        items.append(button);
    }
    if (!choices.length)
    {
        items.textContent = "No other channels";
    }
    groups__show_submenu(menu, item);
}

/** Ask for password bypass before presenting the password form. */
function moderation__join_channel(channel_id)
{
    moderation.pending_join = null;
    let channel = channel_tree__get_channel_by_id(g_channel_list, channel_id);
    if (!channel || channel_id === Number(g_current_channel_id))
    {
        return;
    }
    g_selected_channel_id = channel_id;
    if (channel.is_using_password && !g_is_local_client_admin)
    {
        moderation.pending_join = channel_id;
        moderation__send("channel_access_request", { channel_id: channel_id });
    }
    else
    {
        moderation__send("join_channel_request", { channel_id: channel_id, channel_password: "" });
    }
}

/** Only our own client drag is accepted; file drops continue through their existing handlers. */
function moderation__setup_drag()
{
    let tree = document.getElementById("channel-list-container");
    let dragging = null;
    function clear()
    {
        tree.querySelectorAll(".channel-drop-target").forEach(function(row)
        {
            row.classList.remove("channel-drop-target");
        });
    }
    tree.addEventListener("dragstart", function(event)
    {
        let row = event.target.closest(".connected-client");
        let target = row && channel_tree__get_client_by_client_id(Number(row.dataset.connectedClientId));
        if (!target || target.is_idle || target.is_music_bot || Number(target.client_id) === Number(g_local_client_id))
        {
            event.preventDefault();
            return;
        }
        dragging = { client_id: target.client_id, identity: target.public_key, channel_id: target.channel_id };
        moderation.clients.delete(Number(target.client_id));
        moderation__request_client_access(target);
        event.dataTransfer.setData("application/x-lemon-client", String(target.client_id));
        event.dataTransfer.effectAllowed = "move";
        UI.delete_contextmenus(true);
    });
    tree.addEventListener("dragover", function(event)
    {
        clear();
        let row = event.target.closest(".single-channel");
        let target = dragging && moderation__target(dragging.client_id, dragging.identity, dragging.channel_id);
        if (row && target && moderation__can_act(target, "move") && Number(row.dataset.channelId) !== Number(target.channel_id))
        {
            event.preventDefault();
            event.dataTransfer.dropEffect = "move";
            row.classList.add("channel-drop-target");
        }
    });
    tree.addEventListener("drop", function(event)
    {
        let row = event.target.closest(".single-channel");
        let target = dragging && moderation__target(dragging.client_id, dragging.identity, dragging.channel_id);
        if (dragging)
        {
            event.preventDefault();
            event.stopPropagation();
        }
        if (row && target && Number(row.dataset.channelId) !== Number(target.channel_id))
        {
            moderation__act(target, "move", { destination_channel_id: Number(row.dataset.channelId) });
        }
        dragging = null;
        clear();
    });
    tree.addEventListener("dragleave", function(event)
    {
        if (!tree.contains(event.relatedTarget))
        {
            clear();
        }
    });
    tree.addEventListener("dragend", function()
    {
        dragging = null;
        clear();
    });
}


/**
 * @brief Receives moderation results and channel rights from the server.
 * @param message Decrypted server message.
 */
function moderation__receive(message)
{
    if (message.type === "channel_group_changed")
    {
        let client = channel_tree__get_client_by_client_id(message.client_id);
        if (client && Number(client.channel_id) === Number(message.channel_id))
        {
            client.channel_group_id = Number(message.channel_group_id) || 0;
            channel_groups__refresh_icons();
            channel_groups__render();
            groups__refresh_assignment_menu();
        }
    }
    else if (message.type === "channel_access")
    {
        moderation.channels.set(Number(message.channel_id), message);
        if (G_HAS_DOM && typeof channel_groups__render_member_controls === "function")
        {
            channel_groups__render_member_controls();
        }
        groups__refresh_assignment_menu();
        let local = channel_tree__get_client_by_client_id(g_local_client_id);
        if (local && Number(local.channel_id) === Number(message.channel_id))
        {
            local.channel_group_id = Number(message.role) || 0;
            channel_groups__refresh_icons();
        }
        if (G_HAS_DOM && moderation.channel_edit === Number(message.channel_id))
        {
            moderation__channel_fields(message.channel_id);
        }
        if (G_HAS_DOM && moderation.pending_join === Number(message.channel_id))
        {
            moderation.pending_join = null;
            if (Number(g_selected_channel_id) === Number(message.channel_id))
            {
                if (message.can_bypass_password === true)
                {
                    moderation__send("join_channel_request", { channel_id: Number(message.channel_id), channel_password: "" });
                }
                else
                {
                    document.getElementById("channel-password-enter-container").style.display = "block";
                    document.getElementById("background-container").style.display = "block";
                }
            }
        }
        groups__refresh_channel_controls();
        moderation__refresh_menu_access();
    }
    else if (message.type === "client_context_access")
    {
        let target = moderation__target(message.client_id, message.identity, message.channel_id);
        if (target)
        {
            moderation.clients.set(Number(message.client_id), message);
            if (G_HAS_DOM && typeof channel_groups__render_member_controls === "function")
            {
                channel_groups__render_member_controls();
            }
            if (G_HAS_DOM)
            {
                let menu = document.querySelector('[data-moderation-client="' + Number(message.client_id) + '"]');
                if (menu)
                {
                    moderation__render_client_menu(menu);
                }
                groups__refresh_assignment_menu();
            }
        }
    }
    else if (G_HAS_DOM && message.type === "moderation_result")
    {
        if (!message.succeeded)
        {
            sounds__play("error");
        }
        if (moderation.dialog && moderation.dialog.open)
        {
            moderation.status.textContent = message.text;
        }
        else if (!message.succeeded || message.text !== "Change applied.")
        {
            utils__custom_alert(message.text);
        }
    }
    else if (G_HAS_DOM && message.type === "ban_list" && groups__has_permission("view_bans")
        && moderation.dialog && moderation.dialog.open && moderation.dialog.dataset.page === "bans")
    {
        moderation.body.replaceChildren();
        for (let ban of message.entries)
        {
            let row = document.createElement("div");
            let label = document.createElement("span");
            label.textContent = (groups__has_permission("view_ip_addresses") && ban.ip_address || ban.identity || "Address hidden")
                + " · " + new Date(ban.timestamp).toLocaleString();
            row.append(label);
            if (groups__has_permission("remove_bans"))
            {
                let remove = moderation__button("Remove", function()
                {
                    if (remove.dataset.confirm === "true")
                    {
                        moderation__send("manage_bans", { remove_token: ban.token });
                    }
                    else
                    {
                        remove.dataset.confirm = "true";
                        remove.textContent = "Confirm removal";
                    }
                });
                row.append(remove);
            }
            moderation.body.append(row);
        }
        if (!message.entries.length)
        {
            moderation.status.textContent = "No bans.";
        }
    }
}

/**
 * @brief Enables only the channel fields the server permits this user to edit.
 * @param channel_id Edited channel, or null when creating a channel.
 */
function moderation__channel_fields(channel_id)
{
    let fields =
    {
        edit_channel_name: ["input-channel-name"],
        edit_channel_description: ["input-channel-description"],
        edit_channel_password: ["input-channel-password", "remove-password"],
        edit_channel_options: ["enable-audio-checkbox", "enable-video-checkbox", "limit-clients-checkbox", "input-max-clients"],
        edit_channel_powers: ["required-talk-power", "required-stream-power", "required-message-power", "required-picture-power", "required-file-power", "required-view-power", "required-create-power"]
    };
    for (let [permission, ids] of Object.entries(fields))
    {
        for (let id of ids)
        {
            let input = document.getElementById("channel-properties-" + id);
            if (input)
            {
                input.disabled = channel_id != null && !moderation__allows(channel_id, permission);
                if (channel_id === 0 && (permission === "edit_channel_password" || id === "limit-clients-checkbox" || id === "input-max-clients"))
                {
                    input.disabled = true;
                }
            }
        }
    }
    if (typeof UI.refresh_channel_type_options === "function")
    {
        UI.refresh_channel_type_options(channel_id);
    }
}

/** Retained entry point for menu creation; restricted rows stay visible. */
function moderation__filter_music_menu()
{
    moderation__refresh_menu_access();
}

/**
 * @brief Shows playback and upload controls separately from track deletion.
 * @param message Song list with the bot ID and playback state.
 */
function moderation__music_controls(message)
{
    let controls = document.getElementById("musicbot-playback-controls");
    if (!controls)
    {
        controls = document.createElement("div");
        controls.id = "musicbot-playback-controls";
        document.getElementById("musicbot-management-song-list-container").before(controls);
    }
    controls.replaceChildren();
    if (groups__has_permission("control_music_playback"))
    {
        for (let [action, label] of [[message.is_paused ? "play" : "pause", message.is_paused ? "Play" : "Pause"], ["next", "Next track"]])
        {
            controls.append(moderation__button(label, function()
            {
                moderation__send("musicbot_playback", { bot_id: Number(message.bot_id), action: action });
            }));
        }
    }
    document.getElementById("musicbot-management-upload-container").hidden = !groups__has_permission("upload_music_tracks");
}
