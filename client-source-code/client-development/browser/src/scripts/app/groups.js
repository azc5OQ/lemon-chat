// This file displays server groups and the controls each user has permission to use.

var groups_window =
{
    selected: null,
    access: null,
    timer: null,
    permissions_signature: "",
    identities: [],
    stored_users_allowed: null,
    stored_users_timer: null,
    advanced: false,
    dirty: false,
    permission: null,
    member: null,
    collapsed: new Set(),
    pending_name: null,
    question: null,
    icon_reading: false,
    icon_group: null,
    icon_selection: "none",
    icon_page: 0,
    icon_page_size: 80,
    icons_uploaded: 0,
    icon_errors: []
};

var groups_last_channel_power_warning = 0;

/**
 * @brief Opens the group list or the groups assigned to a selected client.
 * @param number client_id -> optional connected client to select
 */
function groups__open(client_id)
{
    if (!g_is_authenticated || !g_server_policy.is_permission_system_enabled
        || (document.body.classList.contains("server-groups-admin-only") && !g_is_local_client_admin))
    {
        return;
    }
    let dialog = document.getElementById("groups-dialog");
    let client = client_id != null ? channel_tree__get_client_by_client_id(Number(client_id)) : null;
    document.getElementById("groups-show-all").checked = !!client;
    document.getElementById("groups-search").value = "";
    document.getElementById("groups-member-search").value = client ? client.username : "";
    groups__refresh();
    if (groups__has_permission("view_group_members"))
    {
        connection__send_message_object({ message: { type: "request_identity_list" } });
    }
    if (!dialog.open)
    {
        dialog.showModal();
    }
    document.getElementById("groups-search").focus();
}

/**
 * @brief Refreshes an open window after related server messages have been handled.
 */
function groups__schedule_refresh()
{
    if (!G_HAS_DOM || groups_window.timer != null)
    {
        return;
    }
    groups_window.timer = setTimeout(function()
    {
        groups_window.timer = null;
        let client = channel_tree__get_client_by_client_id(g_local_client_id);
        let allowed = g_is_client_list_retrieved && !g_fast_reconnect.resumed
            && g_server_policy.is_stored_client_list_allowed && groups__is_allowed(client, "view_stored_users");
        if (allowed !== groups_window.stored_users_allowed)
        {
            groups_window.stored_users_allowed = allowed;
            if (allowed)
            {
                client_msg.send_request_stored_clients();
            }
            else
            {
                g_offline_client_list = [];
                UI.schedule_member_list_sync();
            }
        }
        if (!groups__has_permission("view_group_members"))
        {
            groups_window.identities = [];
        }
        if (document.getElementById("groups-dialog").open)
        {
            groups__refresh();
        }
    }, 80);
}

/**
 * @brief Checks the current connection before allowing changes to groups.
 * @return boolean -> true when at least one group management action is allowed
 */
function groups__can_manage()
{
    return g_server_policy.is_permission_system_enabled && ["create_groups", "edit_groups", "delete_groups",
        "assign_groups", "remove_groups", "edit_group_permissions", "manage_icons"].some(groups__has_permission);
}

/**
 * @brief Adds a group's server supplied icon and name to a row.
 * @param Element row -> destination row
 * @param object group -> server group
 */
function groups__label(row, group)
{
    let icon = group.has_icon ? channel_tree__get_icon_by_icon_id(group.group_linked_icon_id) : null;
    let image = document.createElement("img");
    image.alt = "";
    image.className = "groups-icon";
    if (icon && /^data:image\/(png|jpeg|gif|webp|svg\+xml);base64,/i.test(icon.base64_icon))
    {
        image.src = icon.base64_icon;
    }
    else
    {
        image.hidden = true;
    }
    let label = document.createElement("span");
    label.textContent = group.group_name;
    row.append(image, label);
}

/**
 * @brief Shows the selected group's permissions, icon and members.
 */
function groups__refresh()
{
    let admin = groups__can_manage();
    let connected = g_is_authenticated;
    let query = document.getElementById("groups-search").value.trim().toLowerCase();
    let list = document.getElementById("groups-list");
    let clients = connected ? g_client_list.filter(function(client)
    {
        return client && !client.is_music_bot;
    }) : [];
    if (groups__has_permission("view_group_members"))
    {
        clients = clients.concat(groups_window.identities.filter(function(identity)
        {
            return !identity.is_online;
        }));
    }
    let entries = connected ? g_groups : [];
    let created = entries.find(function(group)
    {
        return group.group_name === groups_window.pending_name;
    });
    if (created)
    {
        groups_window.selected = created.group_id;
        groups_window.pending_name = null;
    }
    let selected = entries.find(function(group)
    {
        return group.group_id === groups_window.selected;
    }) || entries[0];
    groups_window.selected = selected ? selected.group_id : null;
    let active_id = document.activeElement && document.activeElement.dataset.groupsEntry;
    let previous_scroll = list.scrollTop;
    list.replaceChildren();
    for (let entry of entries)
    {
        if (!(entry.group_name + " " + entry.group_id).toLowerCase().includes(query))
        {
            continue;
        }
        let button = document.createElement("button");
        button.type = "button";
        button.className = "groups-list-entry";
        button.dataset.groupsEntry = String(entry.group_id);
        button.setAttribute("aria-pressed", String(groups_window.selected === entry.group_id));
        button.title = entry.group_name + " (group " + entry.group_id + ")"
            + (entry.is_default_guest ? ". Assigned automatically to users without another group." : "");
        groups__label(button, entry);
        let id = document.createElement("small");
        id.textContent = String(entry.group_id);
        button.appendChild(id);
        button.onclick = async function()
        {
            if (groups_window.selected !== entry.group_id && groups_window.dirty
                && !await groups__ask("Discard the permission changes for this group?", "Discard"))
            {
                return;
            }
            if (!g_groups.includes(entry))
            {
                return;
            }
            groups_window.selected = entry.group_id;
            groups_window.member = null;
            document.getElementById("groups-member-search").value = "";
            groups__refresh();
        };
        list.appendChild(button);
    }
    list.scrollTop = previous_scroll;
    if (active_id != null)
    {
        let previous = list.querySelector('[data-groups-entry="' + active_id + '"]');
        if (previous)
        {
            previous.focus({ preventScroll: true });
        }
    }
    document.getElementById("groups-detail-title").textContent = selected ? selected.group_name : "Permissions";
    document.getElementById("groups-add").disabled = !groups__has_permission("create_groups");
    document.getElementById("groups-rename").disabled = !groups__can_change_group("edit_groups", selected);
    document.getElementById("groups-delete").disabled = !groups__can_change_group("delete_groups", selected);
    document.getElementById("groups-save").hidden = !admin;
    document.getElementById("groups-save-note").hidden = !admin;
    if (!groups__has_permission("create_groups"))
    {
        document.getElementById("groups-create").hidden = true;
    }
    groups__render_icons(selected, groups__can_change_group("edit_groups", selected));
    groups__render_permissions(selected, groups__can_change_group("edit_group_permissions", selected));
    groups__render_members(selected, clients, groups__has_permission("view_group_members"));
}

/**
 * @brief Lists members when the user has permission to view them.
 * @param object selected -> selected group
 * @param Array entries -> connected users and stored identities
 * @param boolean admin -> whether editing is allowed
 */
function groups__render_members(selected, entries, admin)
{
    let container = document.getElementById("groups-members");
    let query = document.getElementById("groups-member-search").value.trim().toLowerCase();
    let show_all = document.getElementById("groups-show-all").checked;
    let focused = document.activeElement && document.activeElement.dataset.groupsAssignment;
    let previous_scroll = container.scrollTop;
    let visible_members = [];
    container.replaceChildren();
    document.getElementById("groups-member-search").disabled = !admin;
    document.getElementById("groups-assignment-options").hidden = !admin;
    document.getElementById("groups-member-add").disabled = !admin || !groups__can_change_group("assign_groups", selected);
    document.getElementById("groups-member-remove").disabled = true;
    document.getElementById("groups-member-note").textContent = selected && selected.is_default_guest
        ? "Users without another group belong here automatically. Adding another group removes this membership. Removing their last group restores it." : "";
    document.getElementById("groups-member-label").textContent = show_all ? "Users" : "Members";
    if (admin && selected)
    {
        visible_members = entries.filter(function(client)
        {
            let name = client.client_id == null ? client.username || "Stored identity"
                : channel_tree__get_display_name_by_client_id(client.client_id, client.username);
            return (show_all || client.group_ids.includes(selected.group_id)) && name.toLowerCase().includes(query);
        });
    }
    let selected_client = visible_members.find(function(client)
    {
        return groups__member_id(client) === groups_window.member;
    });
    if (!selected_client)
    {
        groups_window.member = null;
    }
    for (let client of visible_members)
    {
        let offline = client.client_id == null;
        let assigned = client.group_ids.includes(selected.group_id);
        let button = document.createElement("button");
        button.type = "button";
        button.className = "groups-member";
        button.dataset.groupsAssignment = groups__member_id(client);
        button.setAttribute("aria-pressed", String(groups_window.member === groups__member_id(client)));
        button.title = assigned ? "Member of this group" : "Not in this group";
        let label = document.createElement("span");
        label.className = "groups-member-name";
        label.textContent = offline ? client.username || "Stored identity"
            : channel_tree__get_display_name_by_client_id(client.client_id, client.username);
        let status = document.createElement("small");
        status.textContent = !assigned ? "Not in group" : offline ? "Offline" : "Online";
        button.append(label, status);
        button.onclick = function()
        {
            groups_window.member = groups__member_id(client);
            groups__render_members(selected, entries, groups__has_permission("view_group_members"));
        };
        container.appendChild(button);
    }
    if (selected_client)
    {
        let assigned = selected_client.group_ids.includes(selected.group_id);
        document.getElementById("groups-member-add").disabled = assigned || !groups__can_change_member(selected_client, selected, true);
        document.getElementById("groups-member-remove").disabled = !assigned || !groups__can_change_member(selected_client, selected, false);
    }
    if (!container.childElementCount)
    {
        let empty = document.createElement("p");
        empty.className = "groups-empty";
        empty.textContent = !admin ? "You do not have permission to view this group's members."
            : "No matching users.";
        container.appendChild(empty);
    }
    container.scrollTop = previous_scroll;
    if (focused != null)
    {
        let previous = Array.from(container.children).find(function(row)
        {
            return row.dataset.groupsAssignment === focused;
        });
        if (previous)
        {
            previous.focus({ preventScroll: true });
        }
    }
}

/**
 * @brief Connects group browsing and administration controls.
 */
function groups__setup()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let dialog = document.getElementById("groups-dialog");
    groups__apply_server_mode();
    g_connection_status_listeners.push(groups__apply_server_mode);
    for (let id of ["groups-menu-button", "server-settings-open-groups", "local-settings-open-groups"])
    {
        document.getElementById(id).onclick = function()
        {
            groups__open();
        };
    }
    document.getElementById("groups-save").onclick = function()
    {
        if (!groups__can_manage() || !groups__validate_permissions())
        {
            return;
        }
        document.getElementById("groups-permission-form").requestSubmit();
        connection__send_message_object({ message: { type: "save_groups" } });
    };
    for (let id of ["groups-close", "groups-done"])
    {
        document.getElementById(id).onclick = function()
        {
            dialog.close();
        };
    }
    document.getElementById("groups-show-all").onchange = groups__refresh;
    document.getElementById("groups-search").oninput = groups__refresh;
    document.getElementById("groups-member-search").oninput = groups__refresh;
    document.getElementById("groups-permission-form").onsubmit = groups__apply_permissions;
    document.getElementById("groups-permission-search").oninput = groups__filter_permissions;
    document.getElementById("groups-modified-only").onchange = groups__filter_permissions;
    document.getElementById("groups-mode").onchange = function(event)
    {
        if (!groups__validate_permissions())
        {
            event.target.value = groups_window.advanced ? "advanced" : "simple";
            return;
        }
        let values = groups__permission_values();
        let dirty = groups_window.dirty;
        groups_window.advanced = event.target.value === "advanced";
        groups_window.permissions_signature = "";
        let group = g_groups.find(function(entry)
        {
            return entry.group_id === groups_window.selected;
        });
        groups__render_permissions(group, groups__can_change_group("edit_group_permissions", group), values);
        groups_window.dirty = dirty;
        document.getElementById("groups-edit-status").textContent = dirty ? "Not applied" : "";
    };
    document.getElementById("groups-reset").onclick = function()
    {
        let control = document.getElementById("groups-permission-form").elements[groups_window.permission];
        if (control && !control.disabled)
        {
            control.value = groups_power_fields.some(function(field)
            {
                return field[0] === control.name;
            }) ? "0" : "default";
            control.dispatchEvent(new Event("input", { bubbles: true }));
            groups__filter_permissions();
        }
    };
    document.getElementById("groups-add").onclick = function()
    {
        if (groups__has_permission("create_groups"))
        {
            document.getElementById("groups-new-type").value = "normal";
            groups__describe_preset();
            document.getElementById("groups-create").hidden = false;
            document.getElementById("groups-new-name").focus();
        }
    };
    document.getElementById("groups-new-type").onchange = groups__describe_preset;
    groups__describe_preset();
    document.getElementById("groups-create-cancel").onclick = function()
    {
        document.getElementById("groups-create").hidden = true;
        document.getElementById("groups-add").focus();
    };
    document.getElementById("groups-create").onsubmit = async function(event)
    {
        event.preventDefault();
        let input = document.getElementById("groups-new-name");
        let name = input.value.trim();
        if (!groups__has_permission("create_groups") || !name || new TextEncoder().encode(name).length >= 32)
        {
            input.setCustomValidity("Use a name between 1 and 31 bytes long.");
            input.reportValidity();
            return;
        }
        if (g_groups.some(function(group)
        {
            return group.group_name === name;
        }))
        {
            input.setCustomValidity("A group with this name already exists.");
            input.reportValidity();
            return;
        }
        if (groups_window.dirty && !await groups__ask("Discard the permission changes before creating a group?", "Discard"))
        {
            return;
        }
        if (!groups__has_permission("create_groups") || g_groups.some(function(group)
        {
            return group.group_name === name;
        }))
        {
            return;
        }
        groups_window.pending_name = name;
        connection__send_message_object(
        {
            message:
            {
                type: "server_settings_add_new_group",
                group_name: name,
                group_type: document.getElementById("groups-new-type").value
            }
        });
        input.value = "";
        document.getElementById("groups-search").value = "";
        document.getElementById("groups-create").hidden = true;
    };
    document.getElementById("groups-new-name").oninput = function(event)
    {
        event.target.setCustomValidity("");
    };
    document.getElementById("groups-delete").onclick = async function()
    {
        if (groups__can_change_group("delete_groups")
            && await groups__ask("Delete the selected server group and remove it from all users? This does not delete its icon.", "Delete group")
            && groups__can_change_group("delete_groups"))
        {
            UI.send_delete_group_request(groups_window.selected);
        }
    };
    document.getElementById("groups-icon-button").onclick = function()
    {
        groups__open_icon_picker();
    };
    document.getElementById("groups-icon-search").oninput = function()
    {
        groups_window.icon_page = 0;
        document.getElementById("groups-icon-options").scrollTop = 0;
        groups__render_icon_page();
    };
    document.getElementById("groups-icon-previous").onclick = function()
    {
        groups_window.icon_page--;
        document.getElementById("groups-icon-options").scrollTop = 0;
        groups__render_icon_page();
    };
    document.getElementById("groups-icon-next").onclick = function()
    {
        groups_window.icon_page++;
        document.getElementById("groups-icon-options").scrollTop = 0;
        groups__render_icon_page();
    };
    document.getElementById("groups-icon-use").onclick = function()
    {
        let group = g_groups.find(function(entry)
        {
            return entry.group_id === groups_window.selected;
        });
        let selection = groups_window.icon_selection;
        if (groups_window.icon_group !== groups_window.selected || !groups__can_change_group("edit_groups", group)
            || (selection !== "none" && !g_icons.some(function(icon)
            {
                return String(icon.id) === selection;
            })))
        {
            return;
        }
        UI.send_set_group_icon_request(group.group_id, selection);
        document.getElementById("groups-icon-picker").close();
        document.getElementById("groups-icon-button").setAttribute("aria-expanded", "false");
        document.getElementById("groups-icon-button").focus();
    };
    document.getElementById("groups-icon-delete-selected").onclick = async function()
    {
        let selection = groups_window.icon_selection;
        let icon = g_icons.find(function(entry)
        {
            return String(entry.id) === selection;
        });
        if (icon && groups__has_permission("manage_icons")
            && await groups__ask("Delete icon " + icon.id + " from the server? Groups and channels using it will lose the image.", "Delete icon")
            && groups__has_permission("manage_icons") && g_icons.includes(icon) && groups_window.icon_selection === selection)
        {
            UI.send_delete_icon_request(icon.id);
        }
    };
    for (let id of ["groups-icon-close", "groups-icon-done"])
    {
        document.getElementById(id).onclick = function()
        {
            document.getElementById("groups-icon-picker").close();
        };
    }
    document.getElementById("groups-icon-picker").onclose = function()
    {
        if (document.getElementById("groups-icon-picker").open)
        {
            return;
        }
        if (groups_window.question && groups_window.question.dialog.id === "groups-icon-picker")
        {
            groups__finish_question(null);
        }
        document.getElementById("groups-icon-button").setAttribute("aria-expanded", "false");
        document.getElementById("groups-icon-options").replaceChildren();
    };
    dialog.addEventListener("close", function()
    {
        if (dialog.open)
        {
            return;
        }
        groups__finish_question(null);
        document.getElementById("groups-icon-picker").close();
    });
    document.getElementById("groups-icon-upload").onclick = function()
    {
        if (groups__has_permission("manage_icons"))
        {
            document.getElementById("groups-icon-file").click();
        }
    };
    document.getElementById("groups-icon-file").onchange = groups__upload_icons;
    document.getElementById("groups-rename").onclick = groups__rename;
    document.getElementById("groups-log-close").onclick = function()
    {
        document.getElementById("groups-log-dialog").close();
    };
    document.getElementById("groups-log-refresh").onclick = groups__show_logs;
    document.getElementById("groups-member-add").onclick = function()
    {
        if (!groups__can_change_group("assign_groups"))
        {
            return;
        }
        if (!groups_window.member)
        {
            document.getElementById("groups-show-all").checked = true;
            groups__refresh();
            document.getElementById("groups-member-note").textContent = "Select a user, then choose Add.";
            document.getElementById("groups-member-search").focus();
            return;
        }
        groups__change_membership(true);
    };
    document.getElementById("groups-member-remove").onclick = function()
    {
        groups__change_membership(false);
    };
}

/**
 * @brief Explains the initial permissions chosen for a new group.
 */
function groups__describe_preset()
{
    let descriptions =
    {
        admin: "Starts with all group permissions. It does not grant password admin access.",
        moderator: "Normal permissions plus kicks, bans, member lists and client details. Kick and ban powers start at 50.",
        normal: "Guest permissions plus permanent channels, saved users, offline messages and viewing group permissions.",
        guest: "Temporary channels, server information and contacting idle clients. Only the default Guest group is assigned automatically."
    };
    let type = document.getElementById("groups-new-type").value;
    document.getElementById("groups-preset-description").textContent = descriptions[type] || "";
}

var groups_permission_fields =
[
    ["create_channel", "Create permanent channels", "Default allows admins only."],
    ["create_temp_channel", "Create temporary channels", "Allows one temporary channel per user. Users with Create permanent channels make permanent channels instead."],
    ["kick", "Kick users", "Default allows admins only. Kick power must reach the target's protection."],
    ["ban", "Ban users", "Default allows admins only. Ban power must reach the target's protection."],
    ["view_stored_users", "View saved users", "Shows saved users, including those who are offline. The server must enable this list. Default denies access."],
    ["send_offline_messages", "Send offline messages", "Lets this user leave encrypted messages for offline recipients. The server must enable offline messages. Default denies access."],
    ["receive_offline_messages", "Receive offline messages", "Lets others leave encrypted messages for this user while offline. The sender also needs permission. Default denies access."],
    ["message_idle_clients", "Message idle clients", "Lets this user send private messages to clients in idle mode. The server must allow messages to idle clients. Default allows access."],
    ["call_idle_clients", "Call idle clients", "Lets this user send a call notification to an idle client. The server must enable idle mode. Default allows access."],
    ["view_group_permissions", "View group permissions", "Shows the permission values of server groups. This does not allow editing them."],
    ["view_group_members", "View group members", "Shows connected and stored members in the Groups window. This does not allow changing memberships."],
    ["create_groups", "Create groups", "Creates groups from a preset within your permissions. Editing their permissions requires a separate right."],
    ["edit_groups", "Edit group names and icons", "Changes a group name or its selected icon when management power reaches the group protection."],
    ["delete_groups", "Delete groups", "Deletes groups within your management power. You cannot delete your own group, the built in Admin group or the default Guest group."],
    ["assign_groups", "Assign groups", "Assigns groups within your management power. You cannot assign yourself or grant rights you do not hold."],
    ["remove_groups", "Remove groups", "Removes groups within your management power. Removing the last group restores the default Guest group."],
    ["edit_group_permissions", "Edit group permissions", "Changes permissions within your management power. You cannot edit your own group or grant rights and powers you do not hold."],
    ["manage_icons", "Upload and delete icons", "Manages shared server images. Selecting an icon for a group requires permission to edit that group."],
    ["view_client_details", "View client connection details", "Shows connection details, including the IP address and identity, in client information."],
    ["view_server_logs", "View server logs", "Reads the server log. Clearing it and changing what is recorded still require admin access."],
    ["view_server_info", "View server information", "Shows information the server publishes, including its banner, uptime and used slots. Allowed by default."]
];
var groups_power_fields =
[
    ["talk_power", "Talk power"],
    ["stream_power", "Video streaming power"],
    ["channel_message_power", "Channel message power"],
    ["channel_picture_power", "Channel picture power"],
    ["kick_power", "Kick power"],
    ["kick_protection", "Protection from kicks"],
    ["ban_power", "Ban power"],
    ["ban_protection", "Protection from bans"],
    ["group_management_power", "Group management power"],
    ["group_protection", "Protection from group changes"]
];

/**
 * @brief Checks group permissions for browser controls. The server checks requests separately.
 * @param client -> client whose groups are checked
 * @param permission -> action name
 */
function groups__is_allowed(client, permission)
{
    if (!g_is_authenticated || !client || client.is_music_bot)
    {
        return false;
    }
    if (client.client_id === g_local_client_id ? g_is_local_client_admin
        : g_server_policy.is_permission_system_enabled && client.group_ids.includes(0))
    {
        return true;
    }
    if (client.client_id === g_local_client_id && groups_window.access && g_server_policy.is_permission_system_enabled)
    {
        return groups_window.access[permission] === true;
    }
    if (!g_server_policy.is_permission_system_enabled)
    {
        return permission === "view_server_info" || permission === "create_temp_channel" || permission === "message_idle_clients" || permission === "call_idle_clients";
    }
    let granted = false;
    let denied = false;
    for (let id of client.group_ids)
    {
        let group = channel_tree__get_group_by_group_id(id);
        if (!group)
        {
            return false;
        }
        let value = group.permissions && group.permissions[permission];
        denied = denied || value === "deny";
        granted = granted || value === "grant";
    }
    return granted || !denied && (permission === "view_server_info" || permission === "create_temp_channel"
        || permission === "message_idle_clients" || permission === "call_idle_clients");
}

/**
 * @brief Reads the highest power supplied by a client's groups.
 * @param client -> client to check
 * @param name -> power or protection name
 */
function groups__power(client, name)
{
    if (client && client.client_id === g_local_client_id && groups_window.access)
    {
        return Number(groups_window.access.powers && groups_window.access.powers[name]) || 0;
    }
    let result = 0;
    for (let id of client.group_ids)
    {
        let group = channel_tree__get_group_by_group_id(id);
        result = Math.max(result, Number(group && group.permissions && group.permissions[name]) || 0);
    }
    return result;
}

/**
 * @brief Checks whether the local user's power reaches the channel's requirement.
 * @param action -> talk, stream, message or picture
 * @param channel_id -> channel receiving the audio or message
 * @return boolean -> whether the user may send
 */
function groups__can_send_to_channel(action, channel_id)
{
    if (!g_server_policy.is_permission_system_enabled || g_is_local_client_admin)
    {
        return true;
    }
    let channel = channel_tree__get_channel_by_id(g_channel_list, channel_id);
    let required = Number(channel && channel["required_" + action + "_power"]) || 0;
    if (required === 0)
    {
        return true;
    }
    let access = groups_window.access;
    if (access && access.temp_channel_id != null && Number(access.temp_channel_id) === Number(channel_id))
    {
        return true;
    }
    let name = action === "talk" || action === "stream" ? action + "_power" : "channel_" + action + "_power";
    let client = channel_tree__get_client_by_client_id(g_local_client_id);
    let power = access ? Number(access.powers && access.powers[name]) || 0 : client ? groups__power(client, name) : 0;
    return power >= required;
}

/**
 * @brief Explains which channel requirement prevents sending.
 * @param action -> talk, stream, message or picture
 * @return string -> explanation for the current channel
 */
function groups__channel_power_message(action)
{
    let channel = channel_tree__get_channel_by_id(g_channel_list, g_current_channel_id);
    let required = Number(channel && channel["required_" + action + "_power"]) || 0;
    return "This channel requires " + action + " power " + required + ". Your server groups do not provide enough power.";
}

/**
 * @brief Explains a denied send and plays the permission warning sound.
 * @param action -> talk, stream, message or picture
 */
function groups__notify_channel_power_denied(action)
{
    let now = Date.now();
    if (!G_HAS_DOM || now - groups_last_channel_power_warning < 1000)
    {
        return;
    }
    groups_last_channel_power_warning = now;
    server_msg.process_access_denied_from_server({ message: { reason: groups__channel_power_message(action) } });
}

/**
 * @brief Updates sending controls when channel requirements or group powers change.
 */
function groups__refresh_channel_controls()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let input = document.getElementById("chat-input-container-text-input");
    let picture = document.getElementById("choose_image_input");
    let channel_chat = g_is_authenticated && typeof g_current_chat_context_id === "string"
        && g_current_chat_context_id.startsWith("chat-context-channel-");
    let message_blocked = channel_chat && !groups__can_send_to_channel("message", g_current_channel_id);
    let picture_blocked = channel_chat && !groups__can_send_to_channel("picture", g_current_channel_id);
    let talk_blocked = g_is_authenticated && !groups__can_send_to_channel("talk", g_current_channel_id);
    let song = document.getElementById("choose-song-file-input");
    if (song)
    {
        song.disabled = talk_blocked;
        let label = document.getElementById("custom-file-upload-button-song");
        if (label)
        {
            label.title = talk_blocked ? groups__channel_power_message("talk") : "Stream an MP3 file";
            label.setAttribute("aria-disabled", String(talk_blocked));
        }
    }
    if (input)
    {
        if (input.dataset.channelPowerPlaceholder == null)
        {
            input.dataset.channelPowerPlaceholder = input.placeholder;
        }
        input.readOnly = message_blocked;
        input.placeholder = message_blocked ? groups__channel_power_message("message") : input.dataset.channelPowerPlaceholder;
        input.title = message_blocked ? groups__channel_power_message("message") : "";
    }
    if (picture && input)
    {
        picture.disabled = input.disabled || picture_blocked;
        let label = document.getElementById("custom-file-upload-button-image");
        if (label)
        {
            label.title = picture_blocked ? groups__channel_power_message("picture") : "Send a picture";
            label.setAttribute("aria-disabled", String(picture.disabled));
        }
    }
    for (let id of ["microphone-push-to-talk-button-touch-device", "microphone-always-broadcasting-audio-button"])
    {
        let button = document.getElementById(id);
        if (button)
        {
            if (button.dataset.channelPowerTitle == null)
            {
                button.dataset.channelPowerTitle = button.title;
            }
            button.title = talk_blocked ? groups__channel_power_message("talk") : button.dataset.channelPowerTitle;
            button.setAttribute("aria-disabled", String(talk_blocked));
        }
    }
    if (talk_blocked && typeof g_is_microphone_active !== "undefined" && g_is_microphone_active
        && typeof voice__process_stop_sending_audio_now === "function")
    {
        voice__process_stop_sending_audio_now();
    }
    if (typeof video__refresh_controls === "function")
    {
        video__refresh_controls();
    }
}

/**
 * @brief Checks whether to offer kick or ban for the selected user.
 * @param target -> selected client
 * @param action -> kick or ban
 */
function groups__can_moderate(target, action)
{
    let sender = channel_tree__get_client_by_client_id(g_local_client_id);
    return !!target && groups__is_allowed(sender, action)
        && (g_is_local_client_admin || (!target.group_ids.includes(0)
            && groups__power(sender, action + "_power") >= groups__power(target, action + "_protection")));
}

/**
 * @brief Builds the permission table and preserves edits when only membership changes.
 * @param group -> selected group
 * @param admin -> whether the user may edit
 */
function groups__render_permissions(group, admin, values)
{
    let signature = JSON.stringify([group && group.group_id, group && group.permissions, admin, groups__has_permission("view_group_permissions")]);
    if (signature === groups_window.permissions_signature)
    {
        return;
    }
    document.getElementById("groups-dialog").classList.toggle("groups-advanced", groups_window.advanced);
    groups_window.permissions_signature = signature;
    groups_window.dirty = false;
    groups_window.permission = null;
    let table = document.getElementById("groups-permissions");
    let editable = admin && group && group.group_id !== 0 && groups__has_permission("view_group_permissions");
    table.replaceChildren();
    document.getElementById("groups-apply").disabled = !editable;
    document.getElementById("groups-reset").disabled = true;
    document.getElementById("groups-edit-status").textContent = "";
    document.getElementById("groups-value-heading").textContent = groups_window.advanced ? "Value" : "Setting";
    document.getElementById("groups-help-title").textContent = "Permissions";
    document.getElementById("groups-description").textContent = !group ? "Connect to a server to view its groups."
        : group.group_id === 0 ? "The admin group has full access. Its permissions cannot be changed."
        : "Allow in any group permits that action. Each power uses the highest value from the user's groups. Select a permission for details.";
    if (!group || !groups__has_permission("view_group_permissions"))
    {
        if (group)
        {
            document.getElementById("groups-description").textContent = "You do not have permission to view this group's permissions.";
        }
        return;
    }
    let categories =
    [
        ["Channels", ["create_channel", "create_temp_channel", "talk_power", "stream_power", "channel_message_power", "channel_picture_power"]],
        ["Kick users", ["kick", "kick_power", "kick_protection"]],
        ["Ban users", ["ban", "ban_power", "ban_protection"]],
        ["Groups", ["view_group_permissions", "view_group_members", "create_groups", "edit_groups", "delete_groups", "assign_groups", "remove_groups", "edit_group_permissions", "group_management_power", "group_protection", "manage_icons"]],
        ["Messaging", ["send_offline_messages", "receive_offline_messages", "message_idle_clients", "call_idle_clients"]],
        ["Information", ["view_stored_users", "view_client_details", "view_server_logs", "view_server_info"]]
    ];
    let fields = groups_permission_fields.concat(groups_power_fields);
    for (let category of categories)
    {
        let heading = document.createElement("tr");
        heading.className = "groups-category";
        heading.dataset.category = category[0];
        let cell = document.createElement("td");
        cell.colSpan = 2;
        let button = document.createElement("button");
        button.type = "button";
        button.textContent = category[0];
        button.onclick = function()
        {
            if (groups_window.collapsed.has(category[0]))
            {
                groups_window.collapsed.delete(category[0]);
            }
            else
            {
                groups_window.collapsed.add(category[0]);
            }
            groups__filter_permissions();
        };
        cell.appendChild(button);
        heading.appendChild(cell);
        table.appendChild(heading);
        for (let name of category[1])
        {
            let field = fields.find(function(entry)
            {
                return entry[0] === name;
            });
            let numeric = field.length === 2;
            let value = values && values[name] != null ? values[name]
                : group.group_id === 0 ? numeric ? 1000 : "grant"
                : group.permissions && group.permissions[name] != null ? group.permissions[name] : numeric ? 0 : "default";
            let row = document.createElement("tr");
            row.className = "groups-permission-row";
            row.dataset.category = category[0];
            row.dataset.permission = name;
            let label_cell = document.createElement("td");
            let label = document.createElement("button");
            label.type = "button";
            label.className = "groups-permission-name";
            label.textContent = field[1];
            label.id = "groups-label-" + name;
            if (groups_window.advanced)
            {
                let identifier = document.createElement("small");
                identifier.textContent = name;
                label.appendChild(identifier);
            }
            let control = document.createElement(numeric && groups_window.advanced ? "input" : "select");
            control.name = name;
            control.disabled = !editable;
            control.setAttribute("aria-labelledby", label.id);
            let channel_power = ["talk_power", "stream_power", "channel_message_power", "channel_picture_power"].includes(name);
            control.title = channel_power ? "Your highest group power must reach the power required by the channel."
                : numeric ? "Power is compared with protection. Higher values give more power or protection."
                : "Default leaves this action unchanged. Allow in any group permits it. Deny blocks it only when no group allows it.";
            if (numeric && groups_window.advanced)
            {
                control.type = "number";
                control.min = "0";
                control.max = "1000";
                control.step = "1";
                control.required = true;
            }
            else if (numeric)
            {
                for (let preset of [["Basic", 0], ["Low", 25], ["Normal", 50], ["High", 75], ["Very high", 100], ["Admin", 1000]])
                {
                    control.appendChild(new Option(preset[0], String(preset[1])));
                }
                if (![0, 25, 50, 75, 100, 1000].includes(Number(value)))
                {
                    control.appendChild(new Option("Custom", String(value)));
                }
            }
            else
            {
                control.append(new Option("Default", "default"), new Option("Allow", "grant"), new Option("Deny", "deny"));
            }
            control.value = String(value);
            control.oninput = function()
            {
                groups_window.dirty = true;
                document.getElementById("groups-edit-status").textContent = "Not applied";
            };
            let describe = function()
            {
                groups_window.permission = name;
                for (let sibling of table.querySelectorAll(".groups-permission-row"))
                {
                    sibling.setAttribute("aria-selected", String(sibling === row));
                }
                document.getElementById("groups-help-title").textContent = field[1];
                document.getElementById("groups-description").textContent = channel_power
                    ? "Your highest group power must reach the channel's requirement to "
                        + (name === "talk_power" ? "send audio" : name === "stream_power" ? "share a screen or video file" : name === "channel_message_power" ? "send channel messages" : "send channel pictures")
                        + ". This does not restrict receiving. Temporary channel owners have power 1000 in their own channel. Basic is 0, Low 25, Normal 50, High 75, Very high 100 and Admin 1000. Advanced accepts whole numbers from 0 to 1000."
                    : name === "group_management_power" || name === "group_protection"
                    ? "Management power must reach the protection of both the group and the affected user. The action also needs its own permission. Values range from 0 to 1000. Admins cannot be managed by other users."
                    : numeric
                    ? "The sender's power must be at least the other user's protection. Basic is 0, Low 25, Normal 50, High 75, Very high 100 and Admin 1000. Advanced lets you enter any whole number from 0 to 1000. Admins are always protected from nonadmins."
                    : field[2];
                document.getElementById("groups-reset").disabled = !editable;
            };
            label.onclick = describe;
            control.onfocus = describe;
            label_cell.appendChild(label);
            let value_cell = document.createElement("td");
            value_cell.appendChild(control);
            row.append(label_cell, value_cell);
            table.appendChild(row);
        }
    }
    groups__filter_permissions();
}

/**
 * @brief Sends all permission choices for the selected group to the server.
 * @param event -> submission of the permission form
 */
function groups__apply_permissions(event)
{
    event.preventDefault();
    if (!groups__can_change_group("edit_group_permissions") || !groups__has_permission("view_group_permissions") || groups_window.selected === 0
        || !groups__validate_permissions())
    {
        return;
    }
    connection__send_message_object(
    {
        message:
        {
            type: "set_group_permissions",
            group_id: groups_window.selected,
            permissions: groups__permission_values()
        }
    });
    document.getElementById("groups-edit-status").textContent = "Sent to server";
}

/**
 * @brief Reads every permission, including rows hidden by the filter.
 * @return object -> values to send to the server
 */
function groups__permission_values()
{
    let values = {};
    let form = document.getElementById("groups-permission-form");
    for (let field of groups_permission_fields.concat(groups_power_fields))
    {
        let control = form.elements[field[0]];
        if (control)
        {
            values[field[0]] = field.length === 2 ? Number(control.value) : control.value;
        }
    }
    return values;
}

/**
 * @brief Shows invalid values before asking the browser to focus them.
 * @return boolean -> whether all entered values are valid
 */
function groups__validate_permissions()
{
    let form = document.getElementById("groups-permission-form");
    if (!form.checkValidity())
    {
        document.getElementById("groups-permission-search").value = "";
        document.getElementById("groups-modified-only").checked = false;
        groups_window.collapsed.clear();
        groups__filter_permissions();
        form.reportValidity();
        return false;
    }
    return true;
}

/**
 * @brief Filters the table without changing permission values.
 */
function groups__filter_permissions()
{
    let table = document.getElementById("groups-permissions");
    let query = document.getElementById("groups-permission-search").value.trim().toLowerCase();
    let modified = document.getElementById("groups-modified-only").checked;
    let any = false;
    for (let category of table.querySelectorAll(".groups-category"))
    {
        let found = false;
        let expanded = !!query || !groups_window.collapsed.has(category.dataset.category);
        for (let row of table.querySelectorAll(".groups-permission-row"))
        {
            if (row.dataset.category !== category.dataset.category)
            {
                continue;
            }
            let control = row.querySelector("input, select");
            let changed = control.value !== "default" && control.value !== "0";
            let matches = (row.dataset.permission + " " + row.firstChild.textContent + " " + row.dataset.category).toLowerCase().includes(query)
                && (!modified || changed);
            row.hidden = !matches || !expanded;
            found = found || matches;
        }
        category.hidden = !found;
        category.querySelector("button").setAttribute("aria-expanded", String(expanded));
        any = any || found;
    }
    document.getElementById("groups-no-permissions").hidden = any;
}

/**
 * @brief Shows the actual icons available on this server.
 * @param group -> selected group
 * @param admin -> whether icons may be changed
 */
function groups__render_icons(group, admin)
{
    let preview = document.getElementById("groups-icon-preview");
    preview.replaceChildren();
    if (group)
    {
        groups__label(preview, group);
    }
    document.getElementById("groups-icon-button").disabled = !group || (!admin && !groups__has_permission("manage_icons"));
    document.getElementById("groups-icon-upload").disabled = !groups__has_permission("manage_icons") || groups_window.icon_reading;
    if (!admin && !groups__has_permission("manage_icons"))
    {
        document.getElementById("groups-icon-picker").close();
        document.getElementById("groups-icon-button").setAttribute("aria-expanded", "false");
    }
    if (groups_window.icon_group !== (group ? group.group_id : null))
    {
        groups_window.icon_group = group ? group.group_id : null;
        groups_window.icon_selection = group && group.has_icon ? String(group.group_linked_icon_id) : "none";
        groups_window.icon_page = 0;
        document.getElementById("groups-icon-search").value = "";
    }
    groups__render_icon_page();
}

/**
 * @brief Draws one page of icons so large server collections do not fill the page with images.
 */
function groups__render_icon_page()
{
    let picker = document.getElementById("groups-icon-picker");
    if (!picker.open)
    {
        return;
    }
    let group = g_groups.find(function(entry)
    {
        return entry.group_id === groups_window.selected;
    });
    let admin = groups__can_change_group("edit_groups", group);
    let can_manage = groups__has_permission("manage_icons");
    let selection = g_icons.find(function(icon)
    {
        return String(icon.id) === groups_window.icon_selection;
    });
    if (!selection)
    {
        groups_window.icon_selection = "none";
    }
    let query = document.getElementById("groups-icon-search").value.trim().toLowerCase();
    let icons = query ? g_icons.filter(function(icon)
    {
        return String(icon.id).toLowerCase().includes(query);
    }) : g_icons;
    let pages = Math.max(1, Math.ceil(icons.length / groups_window.icon_page_size));
    groups_window.icon_page = Math.max(0, Math.min(groups_window.icon_page, pages - 1));
    let start = groups_window.icon_page * groups_window.icon_page_size;
    let container = document.getElementById("groups-icon-options");
    let focused = container.contains(document.activeElement) ? document.activeElement.dataset : null;
    let focused_id = focused ? focused.iconId : null;
    let previous_scroll = container.scrollTop;
    let choices = document.createDocumentFragment();
    for (let icon of [null].concat(icons.slice(start, start + groups_window.icon_page_size)))
    {
        let button = document.createElement("button");
        button.type = "button";
        button.dataset.iconId = icon ? String(icon.id) : "none";
        button.title = icon ? "Icon " + icon.id : "No icon";
        button.setAttribute("aria-label", button.title);
        button.setAttribute("aria-pressed", String(groups_window.icon_selection === (icon ? String(icon.id) : "none")));
        button.disabled = !group || (!admin && !can_manage);
        if (icon)
        {
            let image = document.createElement("img");
            image.className = "groups-icon";
            image.alt = "";
            if (/^data:image\/(png|jpeg|gif|webp|svg\+xml);base64,/i.test(icon.base64_icon))
            {
                image.src = icon.base64_icon;
            }
            button.appendChild(image);
        }
        else
        {
            button.textContent = "None";
        }
        button.onclick = function()
        {
            if (groups_window.selected === group.group_id
                && (groups__can_change_group("edit_groups", group) || groups__has_permission("manage_icons")))
            {
                groups_window.icon_selection = icon ? String(icon.id) : "none";
                groups__render_icon_page();
            }
        };
        let entry = document.createElement("div");
        entry.className = "groups-icon-choice";
        entry.appendChild(button);
        choices.appendChild(entry);
    }
    container.replaceChildren(choices);
    container.scrollTop = previous_scroll;
    if (focused_id != null)
    {
        let restored = Array.from(container.querySelectorAll("[data-icon-id]")).find(function(button)
        {
            return button.dataset.iconId === focused_id;
        });
        if (restored)
        {
            restored.focus({ preventScroll: true });
        }
    }
    document.getElementById("groups-icon-previous").disabled = groups_window.icon_page === 0;
    document.getElementById("groups-icon-next").disabled = groups_window.icon_page + 1 >= pages;
    document.getElementById("groups-icon-page").textContent = "Page " + (groups_window.icon_page + 1) + " of " + pages
        + " · " + icons.length + (query ? " matches" : " icons");
    let preview = document.getElementById("groups-icon-selected-preview");
    preview.replaceChildren();
    if (selection && /^data:image\/(png|jpeg|gif|webp|svg\+xml);base64,/i.test(selection.base64_icon))
    {
        let image = document.createElement("img");
        image.className = "groups-icon";
        image.alt = "";
        image.src = selection.base64_icon;
        preview.appendChild(image);
    }
    document.getElementById("groups-icon-selected-label").textContent = selection ? "Icon " + selection.id : "No icon";
    document.getElementById("groups-icon-delete-selected").disabled = !selection || !can_manage;
    document.getElementById("groups-icon-delete-selected").hidden = !can_manage;
    document.getElementById("groups-icon-use").disabled = !admin || !group;
    document.getElementById("groups-icon-note").textContent = g_icons.length ? "Choose an icon, then Use icon. Choose None to remove the group's icon."
        : "No icons yet. Use Upload to add PNG, JPEG or WebP images.";
    if (g_icons.length && !icons.length)
    {
        document.getElementById("groups-icon-note").textContent = "No icons match this ID. Clear the search to see all icons.";
    }
}

/**
 * @brief Opens the server icons in a separate window for the selected group.
 */
function groups__open_icon_picker()
{
    let group = g_groups.find(function(entry)
    {
        return entry.group_id === groups_window.selected;
    });
    if (!g_is_authenticated || !g_server_policy.is_permission_system_enabled || !group
        || (!groups__can_change_group("edit_groups", group) && !groups__has_permission("manage_icons")))
    {
        return;
    }
    let picker = document.getElementById("groups-icon-picker");
    let opening = !picker.open;
    document.getElementById("groups-icon-title").textContent = "Icon for " + group.group_name;
    if (opening)
    {
        let selected_index = group.has_icon ? g_icons.findIndex(function(icon)
        {
            return icon.id === group.group_linked_icon_id;
        }) : -1;
        groups_window.icon_group = group.group_id;
        groups_window.icon_selection = group.has_icon ? String(group.group_linked_icon_id) : "none";
        groups_window.icon_page = Math.floor(Math.max(0, selected_index) / groups_window.icon_page_size);
        document.getElementById("groups-icon-search").value = "";
        picker.showModal();
    }
    groups__render_icons(group, groups__can_change_group("edit_groups", group));
    if (opening)
    {
        let container = document.getElementById("groups-icon-options");
        let selected = container.querySelector('[aria-pressed="true"]');
        container.scrollTop = 0;
        if (selected)
        {
            let bounds = container.getBoundingClientRect();
            let choice_bounds = selected.getBoundingClientRect();
            container.scrollTop = Math.max(0, choice_bounds.top - bounds.top - (container.clientHeight - choice_bounds.height) / 2);
        }
        document.getElementById("groups-icon-search").focus({ preventScroll: true });
    }
    document.getElementById("groups-icon-button").setAttribute("aria-expanded", "true");
}

/**
 * @brief Resizes selected icons in a separate thread before adding them to the upload queue.
 * @param event -> selected files from the Groups window
 */
async function groups__upload_icons(event)
{
    let files = Array.from(event.target.files || []);
    let generation = g_worker_connection_generation;
    event.target.value = "";
    if (!groups__has_permission("manage_icons") || !files.length || groups_window.icon_reading)
    {
        return;
    }
    groups_window.icon_reading = true;
    groups_window.icons_uploaded = 0;
    groups_window.icon_errors = [];
    groups__open_icon_picker();
    document.getElementById("groups-icon-upload").disabled = true;
    groups__icon_upload_status();
    for (let file of files)
    {
        if (generation !== g_worker_connection_generation || !groups__has_permission("manage_icons"))
        {
            break;
        }
        try
        {
            let data = await image_upload__prepare(file, "icon", g_server_policy.icon_max_size);
            if (generation !== g_worker_connection_generation || !groups__has_permission("manage_icons"))
            {
                break;
            }
            g_icon_upload_queue.push(data);
            server_settings_tab__send_next_queued_icon_upload();
        }
        catch (error)
        {
            groups_window.icon_errors.push(file.name + ": " + (error.message || "Could not read this file."));
        }
    }
    groups_window.icon_reading = false;
    document.getElementById("groups-icon-upload").disabled = !groups__has_permission("manage_icons");
    groups__icon_upload_status();
}

/**
 * @brief Counts an icon only after the server confirms its upload.
 */
function groups__icon_uploaded()
{
    groups_window.icons_uploaded++;
    groups__icon_upload_status();
}

/**
 * @brief Shows upload progress and any file errors beside the icon controls.
 */
function groups__icon_upload_status()
{
    let pending = groups_window.icon_reading || g_icon_upload_in_flight_base64 != null || g_icon_upload_queue.length > 0;
    let status = pending ? "Uploading icons… " : "";
    if (groups_window.icons_uploaded)
    {
        status += groups_window.icons_uploaded + " uploaded. ";
    }
    document.getElementById("groups-icon-status").textContent = status + groups_window.icon_errors.join(" ");
}

/**
 * @brief Identifies a connected user or a stored identity in the member list.
 * @param client -> connected user or stored identity
 */
function groups__member_id(client)
{
    return client.client_id == null ? "identity:" + client.public_key_hash : "client:" + client.client_id;
}

/**
 * @brief Adds or removes the selected user from the selected group.
 * @param add -> true to assign the group, false to remove it
 */
function groups__change_membership(add)
{
    if (!groups__has_permission("view_group_members") || !groups__can_change_group(add ? "assign_groups" : "remove_groups"))
    {
        return;
    }
    let client = g_client_list.concat(groups_window.identities.filter(function(identity)
    {
        return !identity.is_online;
    })).find(function(entry)
    {
        return entry && !entry.is_music_bot && groups__member_id(entry) === groups_window.member;
    });
    if (!client || !groups__can_change_member(client, g_groups.find(function(group)
    {
        return group.group_id === groups_window.selected;
    }), add) || client.group_ids.includes(groups_window.selected) === add)
    {
        return;
    }
    let message = client.client_id == null
        ? { type: "modify_identity_group", public_key_hash: client.public_key_hash, group_id: groups_window.selected, add: add }
        : { type: add ? "add_group_to_client" : "remove_group_from_client", client_id: client.client_id, group_id: groups_window.selected };
    connection__send_message_object({ message: message });
    document.getElementById("groups-member-note").textContent = "Sent to server.";
}

/**
 * @brief Hides server group controls until connected to a server that enables them.
 */
function groups__apply_server_mode()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    if (!g_is_authenticated)
    {
        groups_window.identities = [];
        document.getElementById("groups-log-text").value = "";
        document.getElementById("server-settings-log-textarea").value = "";
        document.getElementById("groups-log-dialog").close();
    }
    let disabled = !g_is_authenticated || !g_server_policy.is_permission_system_enabled;
    document.body.classList.toggle("server-groups-disabled", disabled);
    document.getElementById("server-settings-general-temp-channels-row").style.display = g_server_policy.is_permission_system_enabled ? "none" : "";
    document.getElementById("server-settings-general-temp-channels-groups-note").style.display = g_server_policy.is_permission_system_enabled ? "" : "none";
    if (disabled)
    {
        if (typeof UI !== "undefined" && typeof UI.select_channel_properties_tab === "function")
        {
            UI.select_channel_properties_tab("channel", false);
        }
        groups__finish_question(null);
        document.getElementById("groups-icon-picker").close();
        let dialog = document.getElementById("groups-dialog");
        if (dialog.open)
        {
            dialog.close();
        }
        document.getElementById("group-icon-picker-popup").style.display = "none";
        let selected = document.querySelector('.server-settings-tab-li-selected[data-action="1"]');
        if (selected)
        {
            document.querySelector('.server-settings-tab-li[data-action="2"]').click();
        }
        g_icon_upload_queue.length = 0;
        g_icon_upload_in_flight_base64 = null;
    }
    groups__refresh_channel_controls();
    groups__schedule_refresh();
}

/**
 * @brief Shows admin controls according to the server's decision, independently of groups.
 * @param is_admin -> whether the current connection has admin access
 */
function groups__apply_admin_status(is_admin)
{
    if (g_is_local_client_admin && is_admin !== true)
    {
        groups_window.access = null;
    }
    g_is_local_client_admin = is_admin === true;
    if (!G_HAS_DOM)
    {
        return;
    }
    let button = document.getElementById("enter-server-settings");
    button.style.display = g_is_local_client_admin ? "block" : "none";
    button.onclick = UI.enter_server_settings_onclick;
    if (!g_is_local_client_admin)
    {
        if (document.body.classList.contains("server-groups-admin-only"))
        {
            groups__finish_question(null);
            document.getElementById("groups-icon-picker").close();
            document.getElementById("groups-dialog").close();
        }
        document.getElementById("server-settings-tab").style.display = "none";
        groups_window.identities = [];
        g_icon_upload_queue.length = 0;
        g_icon_upload_in_flight_base64 = null;
    }
    server_settings_tab__apply_rename_policy_to_ui();
    groups__refresh_channel_controls();
    groups__schedule_refresh();
}

/**
 * @brief Checks a right for the current connection.
 * @param permission -> permission name
 */
function groups__has_permission(permission)
{
    if (g_is_authenticated && g_is_local_client_admin)
    {
        return true;
    }
    if (g_server_policy.is_permission_system_enabled && groups_window.access == null)
    {
        return false;
    }
    return groups__is_allowed(channel_tree__get_client_by_client_id(g_local_client_id), permission);
}

/**
 * @brief Checks the controls the server enabled for this group.
 * @param permission -> group action
 * @param group -> group to check, or the selected group
 */
function groups__can_change_group(permission, group)
{
    if (!group)
    {
        group = g_groups.find(function(entry)
        {
            return entry.group_id === groups_window.selected;
        });
    }
    if (group && ((permission === "delete_groups" && (group.group_id === 0 || group.is_default_guest))
        || (group.is_default_guest && (permission === "assign_groups" || permission === "remove_groups"))))
    {
        return false;
    }
    return !!group && g_server_policy.is_permission_system_enabled && groups__has_permission(permission)
        && (g_is_local_client_admin || !!(group.access && group.access[permission]));
}

/**
 * @brief Prevents offering membership changes to oneself or to an admin.
 * @param client -> selected connected user or stored identity
 * @param group -> selected group
 * @param add -> true to assign, false to remove
 */
function groups__can_change_member(client, group, add)
{
    return !!client && groups__can_change_group(add ? "assign_groups" : "remove_groups", group)
        && (g_is_local_client_admin || (client.client_id !== g_local_client_id && !client.group_ids.includes(0)));
}

/**
 * @brief Applies current server permissions and removes information the user may no longer view.
 * @param access -> permissions for this connection
 */
function groups__apply_access(access)
{
    let previously_allowed = groups__has_permission("view_group_members");
    groups_window.access = access;
    groups__schedule_stored_users_refresh();
    if (!G_HAS_DOM)
    {
        return;
    }
    groups__refresh_channel_controls();
    if (!groups__has_permission("view_group_members"))
    {
        groups_window.identities = [];
    }
    else if (!previously_allowed && document.getElementById("groups-dialog").open)
    {
        connection__send_message_object({ message: { type: "request_identity_list" } });
    }
    if (!groups__has_permission("manage_icons"))
    {
        g_icon_upload_queue.length = 0;
        g_icon_upload_in_flight_base64 = null;
    }
    if (!groups__has_permission("view_server_logs"))
    {
        document.getElementById("groups-log-text").value = "";
        document.getElementById("server-settings-log-textarea").value = "";
        document.getElementById("groups-log-dialog").close();
    }
    if (!groups__has_permission("view_server_info"))
    {
        server_info__apply({ type: "server_info", visibility: {} });
    }
    if (!groups__has_permission("view_client_details"))
    {
        for (let field of ["connected", "ip", "country", "last-action", "identity"])
        {
            let element = document.getElementById("client-info-" + field);
            if (element)
            {
                element.textContent = "";
            }
        }
    }
    groups__schedule_refresh();
}

/**
 * @brief Renames the selected group without changing its permissions.
 */
async function groups__rename()
{
    let group = g_groups.find(function(entry)
    {
        return entry.group_id === groups_window.selected;
    });
    if (!groups__can_change_group("edit_groups", group))
    {
        return;
    }
    let name = await groups__ask("Rename the selected server group.", "Rename", group.group_name);
    if (name == null || !groups__can_change_group("edit_groups"))
    {
        return;
    }
    connection__send_message_object({ message: { type: "rename_group", group_id: group.group_id, group_name: name } });
}

/**
 * @brief Asks for confirmation inside the current window while messages keep processing.
 * @param question -> text describing the action
 * @param action -> label for the confirmation button
 * @param name -> current group name for a rename, or null for confirmation only
 * @return Promise -> confirmed name, true for confirmation, or null for cancellation
 */
function groups__ask(question, action, name = null)
{
    groups__finish_question(null);
    let icon_dialog = document.getElementById("groups-icon-picker");
    let dialog = icon_dialog.open ? icon_dialog : document.getElementById("groups-dialog");
    if (!dialog.open)
    {
        return Promise.resolve(null);
    }
    return new Promise(function(resolve)
    {
        let form = document.createElement("form");
        let description = document.createElement("p");
        let input = name == null ? null : document.createElement("input");
        let error = document.createElement("p");
        let actions = document.createElement("div");
        let accept = document.createElement("button");
        let cancel = document.createElement("button");
        let previous_focus = document.activeElement;
        let previous_inert = [];
        form.className = "groups-question";
        form.setAttribute("role", "alertdialog");
        form.setAttribute("aria-label", question);
        description.textContent = question;
        form.appendChild(description);
        if (input)
        {
            input.type = "text";
            input.value = name;
            input.maxLength = 31;
            input.setAttribute("aria-label", "Group name");
            form.appendChild(input);
            error.className = "groups-question-error";
            error.setAttribute("role", "alert");
            error.hidden = true;
            form.appendChild(error);
            input.oninput = function()
            {
                error.hidden = true;
                input.removeAttribute("aria-invalid");
            };
        }
        actions.className = "groups-question-actions";
        accept.type = "submit";
        accept.textContent = action;
        cancel.type = "button";
        cancel.textContent = "Cancel";
        actions.append(accept, cancel);
        form.appendChild(actions);
        for (let sibling of dialog.children)
        {
            if (!sibling.classList.contains("groups-titlebar"))
            {
                previous_inert.push([sibling, sibling.inert]);
                sibling.inert = true;
            }
        }
        groups_window.question =
        {
            form: form,
            dialog: dialog,
            resolve: resolve,
            previous_focus: previous_focus,
            previous_inert: previous_inert,
            generation: g_worker_connection_generation,
            group_id: groups_window.selected,
            group: g_groups.find(function(group)
            {
                return group.group_id === groups_window.selected;
            })
        };
        form.onsubmit = function(event)
        {
            event.preventDefault();
            let value = input ? input.value.trim() : true;
            if (input && (!value || new TextEncoder().encode(value).length >= 32))
            {
                error.textContent = "Use a group name between 1 and 31 bytes long.";
                error.hidden = false;
                input.setAttribute("aria-invalid", "true");
                input.focus();
                return;
            }
            groups__finish_question(value);
        };
        cancel.onclick = function()
        {
            groups__finish_question(null);
        };
        form.onkeydown = function(event)
        {
            if (event.key === "Escape")
            {
                event.preventDefault();
                event.stopPropagation();
                groups__finish_question(null);
            }
        };
        dialog.querySelector(".groups-titlebar").after(form);
        (input || cancel).focus();
        if (input)
        {
            input.select();
        }
    });
}

/**
 * @brief Removes a pending question and rejects answers from an old connection or selection.
 * @param value -> confirmed name, true for confirmation, or null for cancellation
 */
function groups__finish_question(value)
{
    let question = groups_window.question;
    if (!question)
    {
        return;
    }
    groups_window.question = null;
    if (!g_is_authenticated || !g_server_policy.is_permission_system_enabled || !question.dialog.open
        || question.generation !== g_worker_connection_generation || question.group_id !== groups_window.selected
        || !g_groups.includes(question.group))
    {
        value = null;
    }
    question.form.remove();
    for (let previous of question.previous_inert)
    {
        previous[0].inert = previous[1];
    }
    if (question.dialog.open && question.previous_focus && question.previous_focus.isConnected)
    {
        question.previous_focus.focus();
    }
    question.resolve(value);
}

/**
 * @brief Keeps public group controls in Hybrid themes and closes them elsewhere for nonadmins.
 * @param themename -> selected theme name
 */
function groups__apply_theme_access(themename)
{
    let admin_only = themename !== "hybrid" && themename !== "hybrid-v2";
    document.body.classList.toggle("server-groups-admin-only", admin_only);
    if (admin_only && !g_is_local_client_admin)
    {
        groups__finish_question(null);
        document.getElementById("groups-icon-picker").close();
        document.getElementById("groups-dialog").close();
    }
}

/**
 * @brief Opens a log viewer without exposing server settings controls.
 */
function groups__show_logs()
{
    if (!groups__has_permission("view_server_logs"))
    {
        return;
    }
    let dialog = document.getElementById("groups-log-dialog");
    if (!dialog.open)
    {
        dialog.showModal();
    }
    connection__send_message_object({ message: { type: "admin_log_request" } });
}

/**
 * @brief Requests one saved user list when several related updates arrive together.
 */
function groups__schedule_stored_users_refresh()
{
    if (groups_window.stored_users_timer != null || !g_is_authenticated
        || !g_server_policy.is_stored_client_list_allowed
        || !groups__is_allowed(channel_tree__get_client_by_client_id(g_local_client_id), "view_stored_users"))
    {
        return;
    }
    groups_window.stored_users_timer = setTimeout(function()
    {
        groups_window.stored_users_timer = null;
        client_msg.send_request_stored_clients();
    }, 500);
}
