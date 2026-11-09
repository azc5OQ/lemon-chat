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
    permissions_group: null,
    permissions_source: "",
    edit_status: "",
    save_pending: null,
    save_timer: null,
    permission: null,
    member: null,
    adding_member: false,
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
    groups_window.adding_member = false;
    document.getElementById("groups-search").value = "";
    document.getElementById("groups-member-search").value = client ? client.username : "";
    groups__refresh();
    channel_groups__render();
    if (groups__has_permission("view_group_members"))
    {
        connection__send_message_object({ message: { type: "request_identity_list" } });
    }
    if (!dialog.open)
    {
        dialog.show();
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
        channel_groups__refresh_icons();
        channel_groups__render();
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
        "assign_groups", "remove_groups", "edit_group_permissions", "upload_icons", "delete_icons"].some(groups__has_permission);
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
    groups__refresh_assignment_menu();
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
            if (groups_window.save_pending)
            {
                return;
            }
            if (groups_window.selected !== entry.group_id && groups_window.dirty
                && !await groups__ask("Discard the permission changes for this group?", "Discard"))
            {
                return;
            }
            if (!g_groups.includes(entry))
            {
                return;
            }
            groups_window.adding_member = false;
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
    groups__update_save_controls();
}

/**
 * @brief Lists members when the user has permission to view them.
 * @param object selected -> selected group
 * @param Array entries -> connected users and stored identities
 * @param boolean admin -> whether editing is allowed
 */
function groups__render_members(selected, entries, admin)
{
    let seen = new Set();
    entries = entries.filter(function(client)
    {
        let identity = groups__member_id(client);
        if (seen.has(identity))
        {
            return false;
        }
        seen.add(identity);
        return true;
    });
    let names = new Map();
    for (let client of entries)
    {
        let name = (client.username || "Stored identity").toLowerCase();
        names.set(name, (names.get(name) || 0) + 1);
    }
    let container = document.getElementById("groups-members");
    let query = document.getElementById("groups-member-search").value.trim().toLowerCase();
    let adding = groups_window.adding_member;
    let focused = document.activeElement && document.activeElement.dataset.groupsAssignment;
    let previous_scroll = container.scrollTop;
    let visible_members = [];
    container.replaceChildren();
    document.getElementById("groups-member-search").disabled = !admin;
    document.getElementById("groups-member-cancel").hidden = !adding;
    document.getElementById("groups-member-add").disabled = !admin || !groups__can_change_group("assign_groups", selected);
    document.getElementById("groups-member-remove").disabled = true;
    document.getElementById("groups-member-note").textContent = selected && selected.is_default_guest
        ? "Users without another group belong here automatically. Adding another group removes this membership. Removing their last group restores it." : "";
    document.getElementById("groups-member-label").textContent = adding ? "Add a member" : "Members";
    if (admin && selected)
    {
        visible_members = entries.filter(function(client)
        {
            let name = client.client_id == null ? client.username || "Stored identity"
                : channel_tree__get_display_name_by_client_id(client.client_id, client.username);
            return (adding ? !client.group_ids.includes(selected.group_id) : client.group_ids.includes(selected.group_id)) && name.toLowerCase().includes(query);
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
        if (!offline && Number(client.client_id) === Number(g_local_client_id))
        {
            label.textContent += " (you)";
        }
        let identity_hash = groups__member_identity_hash(client);
        if (names.get((client.username || "Stored identity").toLowerCase()) > 1)
        {
            label.textContent += " \u00b7 " + (identity_hash ? identity_hash.slice(0, 12) : "client " + client.client_id);
        }
        button.title += identity_hash ? "\nIdentity: " + identity_hash : "";
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
        document.getElementById("groups-member-add").disabled = adding
            ? !groups__can_change_member(selected_client, selected, true) : !groups__can_change_group("assign_groups", selected);
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
    for (let id of ["groups-menu-button", "server-settings-groups-button", "local-settings-open-groups"])
    {
        document.getElementById(id).onclick = function()
        {
            if (id === "server-settings-groups-button")
            {
                UI.close_button_server_settings_onclick();
            }
            groups__open();
        };
    }
    document.getElementById("groups-save").onclick = groups__save_changes;
    for (let id of ["groups-close", "groups-done"])
    {
        document.getElementById(id).onclick = function()
        {
            dialog.close();
        };
    }
    document.getElementById("groups-member-cancel").onclick = function()
    {
        groups_window.adding_member = false;
        groups_window.member = null;
        document.getElementById("groups-member-search").value = "";
        groups__refresh();
    };
    document.getElementById("groups-search").oninput = groups__refresh;
    document.getElementById("groups-member-search").oninput = groups__refresh;
    let permission_form = document.getElementById("groups-permission-form");
    permission_form.onsubmit = function(event)
    {
        event.preventDefault();
    };
    permission_form.oninput = groups__mark_dirty;
    permission_form.onchange = groups__mark_dirty;
    document.getElementById("groups-permission-search").oninput = groups__filter_permissions;
    document.getElementById("groups-modified-only").onchange = groups__filter_permissions;
    document.getElementById("groups-mode").onchange = function(event)
    {
        if (groups_window.save_pending || !groups__validate_permissions())
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
        groups__set_edit_status(dirty ? "Unsaved changes" : groups_window.edit_status);
        groups__update_save_controls();
    };
    document.getElementById("groups-reset").onclick = function()
    {
        let control = document.getElementById("groups-permission-form").elements[groups_window.permission];
        if (control && !control.disabled)
        {
            control.value = control.dataset.savedValue;
            let grant = control.closest("tr").querySelector("[data-permission-grant]");
            if (grant)
            {
                grant.value = grant.dataset.savedValue;
            }
            for (let flag of control.closest("tr").querySelectorAll("[data-permission-flag]"))
            {
                flag.checked = flag.dataset.savedValue === "true";
            }
            control.dispatchEvent(new Event("input", { bubbles: true }));
            groups__filter_permissions();
        }
    };
    document.getElementById("groups-add").onclick = function()
    {
        if (!groups_window.save_pending && groups__has_permission("create_groups"))
        {
            document.getElementById("groups-new-type").value = "normal";
            document.getElementById("groups-create").hidden = false;
            document.getElementById("groups-new-name").focus();
        }
    };
    document.getElementById("groups-create-cancel").onclick = function()
    {
        document.getElementById("groups-create").hidden = true;
        document.getElementById("groups-add").focus();
    };
    document.getElementById("groups-create").onsubmit = async function(event)
    {
        event.preventDefault();
        if (groups_window.save_pending)
        {
            return;
        }
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
        if (!groups_window.save_pending && groups__can_change_group("delete_groups")
            && await groups__ask("Delete the selected server group and remove it from all users? This does not delete its icon.", "Delete group")
            && !groups_window.save_pending && groups__can_change_group("delete_groups"))
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
        if (icon && groups__has_permission("delete_icons")
            && await groups__ask("Delete icon " + icon.id + " from the server? Groups and channels using it will lose the image.", "Delete icon")
            && groups__has_permission("delete_icons") && g_icons.includes(icon) && groups_window.icon_selection === selection)
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
        if (groups__has_permission("upload_icons"))
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
        if (!groups_window.adding_member || !groups_window.member)
        {
            groups_window.adding_member = true;
            groups_window.member = null;
            document.getElementById("groups-member-search").value = "";
            groups__refresh();
            document.getElementById("groups-member-note").textContent = "Select a user, then choose Add.";
            document.getElementById("groups-member-search").focus();
            return;
        }
        groups__change_membership(true);
        groups_window.adding_member = false;
        groups_window.member = null;
        groups__refresh();
    };
    document.getElementById("groups-member-remove").onclick = function()
    {
        groups__change_membership(false);
    };
}

var groups_permission_fields =
[
    ["create_channel", "Create permanent channels", "Enables permanent channel creation."],
    ["create_temp_channel", "Create temporary channels", "Enables one temporary channel per user. Users with Create permanent channels make permanent channels instead."],
    ["kick", "Kick users", "Kick power must reach the target's protection."],
    ["ban", "Ban users", "Ban power must reach the target's protection."],
    ["view_stored_users", "View saved users", "Shows saved users, including those who are offline. The server must enable this list."],
    ["send_offline_messages", "Send offline messages", "Lets this user leave encrypted messages for offline recipients. The server must enable offline messages."],
    ["receive_offline_messages", "Receive offline messages", "Lets others leave encrypted messages for this user while offline. The sender also needs permission."],
    ["message_idle_clients", "Message idle clients", "Lets this user send private messages to clients in idle mode. The server must allow messages to idle clients."],
    ["call_idle_clients", "Call idle clients", "Lets this user send a call notification to an idle client. The server must enable idle mode."],
    ["view_group_permissions", "View group permissions", "Shows the permission values of server groups. This does not allow editing them."],
    ["view_group_members", "View group members", "Shows connected and stored members in the Groups window. This does not allow changing memberships."],
    ["create_groups", "Create groups", "Creates groups from a preset within your permissions. Editing their permissions requires a separate right."],
    ["edit_groups", "Edit group names and icons", "Changes a group name or its selected icon when management power reaches the group protection."],
    ["delete_groups", "Delete groups", "Deletes groups within your management power. You cannot delete your own group, the built in Admin group or the default Guest group."],
    ["assign_groups", "Assign groups", "Assigns groups within your management power. You cannot assign yourself or grant rights you do not hold."],
    ["remove_groups", "Remove groups", "Removes groups within your management power. Removing the last group restores the default Guest group."],
    ["edit_group_permissions", "Edit group permissions", "Changes permissions within your management power. You cannot edit your own group or grant rights and powers you do not hold."],
    ["upload_icons", "Upload server icons", "Adds shared server icons. Selecting an icon for a group requires permission to edit that group."],
    ["view_client_details", "View client details", "Shows identity, country and last activity. IP addresses and connection duration have separate permissions."],
    ["view_client_connection_time", "View connection duration", "Shows how long a client has been connected to this server. Enabled for guests by default."],
    ["view_server_logs", "View server logs", "Reads the server log. Clearing it and changing what is recorded still require admin access."],
    ["view_server_info", "View server information", "Shows information the server publishes, including its banner, uptime and used slots. Enabled by default."],
    ["delete_icons", "Delete server icons", "Deletes shared server icons. Does not grant upload rights."],
    ["upload_emojis", "Upload server emojis", "Adds server reaction images, including supported GIF and WebP images."],
    ["delete_emojis", "Delete server emojis", "Removes emojis from the server picker."],
    ["view_ip_addresses", "View IP addresses", "Shows addresses in client details and the ban list. Viewing those lists needs its own permission."],
    ["view_bans", "View bans", "Reads the ban list without allowing new bans."],
    ["remove_bans", "Remove bans", "Removes existing bans. Does not grant the right to ban users."],
    ["move_clients", "Move users between channels", "Requires move power to reach the target protection. Channel rights must cover both channels."],
    ["kick_from_channel", "Kick users from a channel", "Moves users to root without disconnecting them. Requires move power."],
    ["mute_clients", "Mute users temporarily", "Mutes microphone and MP3 audio in the selected channel, subject to moderation protection."],
    ["stop_client_streams", "Stop video streams", "Ends an active screen share or video stream, subject to moderation protection."],
    ["edit_channel_name", "Edit channel names", "Changes the name without granting other channel edits."],
    ["edit_channel_description", "Edit channel descriptions", "Changes the description without granting other channel edits."],
    ["bypass_channel_password", "Bypass channel passwords", "Enter password-protected channels without a password. Channel groups grant this only for their assigned channel; capacity limits still apply."],
    ["edit_channel_password", "Edit channel passwords", "Sets, replaces or removes a channel password."],
    ["edit_channel_powers", "Edit channel power requirements", "Changes the required talk, message, picture and streaming powers."],
    ["edit_channel_options", "Edit channel options and icons", "Changes audio, streaming, client limits and the selected channel icon."],
    ["delete_channel", "Delete channels", "Deletes channels. Every child channel must also be within your permissions."],
    ["assign_channel_groups", "Assign channel groups", "Assigns or removes a channel group within your management power. It applies only in that channel."],
    ["send_direct_pictures", "Send direct pictures", "Enables picture uploads to individual users. Enabled by default."],
    ["send_direct_files", "Send direct files", "Enables file uploads to individual users. Enabled by default."],
    ["send_channel_pictures", "Send channel pictures", "Enables channel picture uploads. Channel picture power also applies. Enabled by default."],
    ["send_channel_files", "Send channel files", "Enables channel file uploads. Enabled by default."],
    ["create_music_bots", "Create music bots", "Adds a music bot when the server enables bot audio."],
    ["delete_music_bots", "Delete music bots", "Removes bots and their uploaded songs."],
    ["upload_music_tracks", "Upload music tracks", "Adds MP3 files to a music bot."],
    ["delete_music_tracks", "Delete music tracks", "Removes songs from a music bot."],
    ["control_music_playback", "Control music playback", "Plays, pauses and skips songs without granting upload or delete rights."],
    ["use_reserved_slots", "Use reserved slots", "May use the slots reserved in server settings. The server total stays unchanged."],
    ["create_channel_groups", "Create channel groups", "Creates channel groups without granting rights you do not hold. Assignment has a separate permission."],
    ["edit_channel_groups", "Edit channel groups", "Changes channel group names, icons and permissions within your management power."],
    ["delete_channel_groups", "Delete channel groups", "Deletes custom channel groups within your management power and removes their assignments."]
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
    ["permission_edit_power", "Permission editing power"],
    ["group_protection", "Protection from group changes"],
    ["move_power", "Move power"],
    ["move_protection", "Protection from moves"],
    ["moderation_power", "Moderation power"],
    ["moderation_protection", "Protection from mutes and stream stops"]
];

/**
 * @brief Resolves a legacy action value using the server's fallback for that action.
 * @param permission Action name.
 * @param value Saved grant, deny or legacy default value.
 */
function groups__action_value(permission, value)
{
    if (value === "grant" || value === "deny")
    {
        return value;
    }
    let allowed = ["view_server_info", "view_client_connection_time", "create_temp_channel",
        "message_idle_clients", "call_idle_clients", "send_direct_pictures", "send_direct_files",
        "send_channel_pictures", "send_channel_files"].includes(permission);
    return allowed ? "grant" : "deny";
}

/**
 * @brief Combines group values using the same optional Negate rule as the server.
 */
function groups__combined_value(client, name, numeric)
{
    let result = 0;
    let negated = false;
    for (let id of client.group_ids || [])
    {
        let group = channel_tree__get_group_by_group_id(id);
        if (!group)
        {
            return numeric ? 0 : 1;
        }
        let permissions = group && group.permissions || {};
        let value = numeric ? Number(permissions[name]) || 0
            : permissions[name] === "grant" ? 2 : permissions[name] === "deny" ? 1 : 0;
        let flag = !!(permissions.flags && permissions.flags[name] && permissions.flags[name].negate);
        if (flag)
        {
            result = negated ? Math.min(result, value) : value;
            negated = true;
        }
        else if (!negated)
        {
            result = Math.max(result, value);
        }
    }
    return result;
}

/**
 * @brief Identifies server permissions that have a channel-scoped counterpart.
 */
function groups__supports_skip(name)
{
    return ["bypass_channel_password", "edit_channel_name", "edit_channel_description", "edit_channel_password", "edit_channel_powers",
        "edit_channel_options", "delete_channel", "assign_channel_groups", "kick_from_channel", "mute_clients",
        "stop_client_streams", "group_management_power", "group_protection", "talk_power", "channel_message_power",
        "channel_picture_power", "stream_power", "move_power", "move_protection", "moderation_power", "moderation_protection"].includes(name);
}

/**
 * @brief Reads the server-wide power used to edit individual group settings.
 */
function groups__editing_power()
{
    return g_is_local_client_admin ? 1000
        : groups__power(channel_tree__get_client_by_client_id(g_local_client_id), "permission_edit_power");
}

/**
 * @brief Missing Grant fields use the same safe default as the server.
 */
function groups__required_edit_power(permissions, name)
{
    let value = permissions && permissions.grants && permissions.grants[name];
    return Number.isInteger(value) && value >= 0 && value <= 1000 ? value : 1000;
}

function groups__can_edit_setting(permissions, name)
{
    return g_is_local_client_admin || groups__editing_power() >= groups__required_edit_power(permissions, name);
}

function groups__setting_hint(permissions, name, editable)
{
    if (!editable && !groups__can_edit_setting(permissions, name))
    {
        return "Requires editing power " + groups__required_edit_power(permissions, name)
            + "; yours is " + groups__editing_power() + ".";
    }
    else if (permissions && permissions.flags && permissions.flags[name]
        && (permissions.flags[name].negate || permissions.flags[name].skip))
    {
        return "Advanced rule active. Switch to Advanced to inspect Negate and Skip.";
    }
    return "";
}

/**
 * @brief Builds the shared server/channel Grant control. Authorization uses the saved value.
 */
function groups__grant_cell(name, label, permissions, draft, editable)
{
    let cell = document.createElement("td");
    let control = document.createElement("input");
    let saved = groups__required_edit_power(permissions, name);
    cell.className = "groups-grant-cell";
    control.type = "number";
    control.min = "0";
    control.max = String(editable ? groups__editing_power() : 1000);
    control.step = "1";
    control.required = true;
    control.dataset.permissionGrant = name;
    control.dataset.savedValue = String(saved);
    control.value = String(draft ? groups__required_edit_power(draft, name) : saved);
    control.disabled = !editable;
    control.setAttribute("aria-label", label + ": Grant (required edit power)");
    control.title = "Required permission editing power: " + saved + ". Your power: " + groups__editing_power()
        + ". Group editing and protection checks also apply. See Permission help.";
    control.oninput = groups__mark_dirty;
    cell.append(control);
    return cell;
}

function groups__read_grant_controls(container, values)
{
    values.grants = {};
    for (let control of container.querySelectorAll("[data-permission-grant]"))
    {
        values.grants[control.dataset.permissionGrant] = Number(control.value);
    }
}

/**
 * @brief Builds an accessible flag control with its original value for reset and filtering.
 */
function groups__flag_cell(name, label, flag, checked, saved, editable)
{
    let cell = document.createElement("td");
    cell.className = "groups-flag-cell";
    let control = document.createElement("input");
    control.type = "checkbox";
    control.dataset.permissionFlag = flag;
    control.dataset.permission = name;
    control.dataset.savedValue = String(!!saved);
    control.checked = !!checked;
    control.disabled = !editable || !g_is_local_client_admin;
    control.setAttribute("aria-label", label + ": " + (flag === "negate" ? "Negate" : "Skip"));
    control.title = flag === "negate" ? "The lowest Negate value wins over ordinary values. Disable + Negate blocks an action."
        : "Ignore channel-group values for this permission. Server-group values still combine normally.";
    control.oninput = groups__mark_dirty;
    cell.append(control);
    return cell;
}

/**
 * @brief Includes active flags in a permission update without inventing implicit values.
 */
function groups__read_flag_controls(container, values)
{
    let flags = {};
    for (let control of container.querySelectorAll("[data-permission-flag]"))
    {
        if (control.checked)
        {
            let name = control.dataset.permission;
            flags[name] = flags[name] || {};
            flags[name][control.dataset.permissionFlag] = true;
        }
    }
    if (Object.keys(flags).length)
    {
        values.flags = flags;
    }
}

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
        return groups__action_value(permission) === "grant";
    }
    let value = groups__combined_value(client, permission, false);
    return value === 2 || value === 0 && groups__action_value(permission) === "grant";
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
    return client ? groups__combined_value(client, name, true) : 0;
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
    let name = action === "talk" || action === "stream" ? action + "_power" : "channel_" + action + "_power";
    let client = channel_tree__get_client_by_client_id(g_local_client_id);
    let power = client ? groups__power(client, name) : 0;
    let channel_access = moderation.channels.get(Number(channel_id));
    if (channel_access)
    {
        power = Number(channel_access.powers && channel_access.powers[name]) || 0;
    }
    else if (access && access.channel_id === Number(channel_id))
    {
        power = Number(access.powers && access.powers[name]) || 0;
    }
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
    return "This channel requires " + action + " power " + required + ". Your effective power in this channel is too low.";
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
    let signature = JSON.stringify([group && group.group_id, group && group.permissions, admin, groups__editing_power(), groups__has_permission("view_group_permissions")]);
    if (signature === groups_window.permissions_signature)
    {
        return;
    }
    let same_group = !!group && groups_window.permissions_group === group.group_id;
    let keep_draft = same_group && groups_window.dirty && groups__has_permission("view_group_permissions");
    let source = JSON.stringify(group && group.permissions);
    if (keep_draft && !values)
    {
        values = groups__permission_values();
    }
    if (!same_group)
    {
        groups__set_edit_status("");
    }
    else if (keep_draft && source !== groups_window.permissions_source && !groups_window.save_pending)
    {
        groups__set_edit_status("Server permissions changed. Review your unsaved changes before saving.");
    }
    document.getElementById("groups-dialog").classList.toggle("groups-advanced", groups_window.advanced);
    groups_window.permissions_signature = signature;
    groups_window.permissions_group = group ? group.group_id : null;
    groups_window.permissions_source = source;
    groups_window.dirty = keep_draft;
    groups_window.permission = null;
    let table = document.getElementById("groups-permissions");
    let editable = admin && group && group.group_id !== 0 && groups__has_permission("view_group_permissions");
    table.replaceChildren();
    document.getElementById("groups-reset").disabled = true;
    groups__set_edit_status(groups_window.edit_status);
    document.getElementById("groups-value-heading").textContent = groups_window.advanced ? "Value" : "Setting";
    document.getElementById("groups-help-title").textContent = "Permissions";
    document.getElementById("groups-description").textContent = !group ? "Connect to a server to view its groups."
        : group.group_id === 0 ? "The admin group has full access. Its permissions cannot be changed."
        : "Grant is the editing power required to change each setting. Enable and the highest power normally win. See Permission help for Grant, Negate and Skip examples.";
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
        ["Channels", ["bypass_channel_password", "create_channel", "create_temp_channel", "edit_channel_name", "edit_channel_description", "edit_channel_password", "edit_channel_powers", "edit_channel_options", "delete_channel", "assign_channel_groups", "talk_power", "stream_power", "channel_message_power", "channel_picture_power"]],
        ["Moderation", ["kick", "kick_power", "kick_protection", "ban", "ban_power", "ban_protection", "view_bans", "remove_bans", "move_clients", "kick_from_channel", "move_power", "move_protection", "mute_clients", "stop_client_streams", "moderation_power", "moderation_protection"]],
        ["Groups", ["view_group_permissions", "view_group_members", "create_groups", "edit_groups", "delete_groups", "assign_groups", "remove_groups", "edit_group_permissions", "group_management_power", "group_protection", "permission_edit_power"]],
        ["Messaging", ["send_direct_pictures", "send_direct_files", "send_channel_pictures", "send_channel_files", "send_offline_messages", "receive_offline_messages", "message_idle_clients", "call_idle_clients"]],
        ["Music bots", ["create_music_bots", "delete_music_bots", "upload_music_tracks", "delete_music_tracks", "control_music_playback"]],
        ["Icons and emojis", ["upload_icons", "delete_icons", "upload_emojis", "delete_emojis"]],
        ["Channel group management", ["create_channel_groups", "edit_channel_groups", "delete_channel_groups"]],
        ["Information and access", ["view_stored_users", "view_client_connection_time", "view_client_details", "view_ip_addresses", "view_server_logs", "view_server_info", "use_reserved_slots"]]
    ];
    let fields = groups_permission_fields.concat(groups_power_fields);
    for (let category of categories)
    {
        let heading = document.createElement("tr");
        heading.className = "groups-category";
        heading.dataset.category = category[0];
        let cell = document.createElement("td");
        cell.colSpan = groups_window.advanced ? 5 : 2;
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
            let saved_value = group.group_id === 0 ? numeric ? 1000 : "grant"
                : group.permissions && group.permissions[name] != null ? group.permissions[name] : numeric ? 0 : "default";
            let value = values && values[name] != null ? values[name] : saved_value;
            if (!numeric)
            {
                saved_value = groups__action_value(name, saved_value);
                value = groups__action_value(name, value);
            }
            let row_editable = editable && groups__can_edit_setting(group.permissions, name);
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
            let setting_hint = groups__setting_hint(group.permissions, name, row_editable);
            label.title = setting_hint || field[2] || field[1];
            row.title = setting_hint;
            if (groups_window.advanced)
            {
                let identifier = document.createElement("small");
                identifier.textContent = name;
                label.appendChild(identifier);
            }
            let control = document.createElement(numeric && groups_window.advanced ? "input" : "select");
            control.name = name;
            control.dataset.savedValue = String(saved_value);
            control.disabled = !row_editable;
            control.setAttribute("aria-labelledby", label.id);
            let channel_power = ["talk_power", "stream_power", "channel_message_power", "channel_picture_power"].includes(name);
            control.title = channel_power ? "Your highest group power must reach the power required by the channel."
                : numeric ? "Power is compared with protection. Higher values give more power or protection."
                : "Enable permits the action. Disable + Negate overrides ordinary Enable values. See Permission help for examples.";
            if (setting_hint)
            {
                control.title = setting_hint + " " + control.title;
            }
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
                for (let preset of [["None", 0], ["Low", 25], ["Normal", 50], ["High", 75], ["Very high", 100], ["Admin", 1000]])
                {
                    control.appendChild(new Option(preset[0], String(preset[1])));
                }
                for (let number of new Set([Number(saved_value), Number(value)]))
                {
                    if (![0, 25, 50, 75, 100, 1000].includes(number))
                    {
                        control.appendChild(new Option("Custom (" + number + ")", String(number)));
                    }
                }
            }
            else
            {
                control.append(new Option("Enable", "grant"), new Option("Disable", "deny"));
            }
            control.value = String(value);
            control.oninput = groups__mark_dirty;
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
                        + ". This does not restrict receiving. Channel owners start with power 1000 from Channel Admin; Negate can restrict it. None is 0, Low 25, Normal 50, High 75, Very high 100 and Admin 1000. Advanced accepts whole numbers from 0 to 1000."
                    : name === "permission_edit_power"
                    ? "Authority to edit individual permission settings. It must reach the saved Grant on each changed row, alongside group editing and management rights. New Admin: 1000, Moderator: 50, Normal: 25, Guest: 0. Any custom group can use any value from 0 to 1000. This power is server-wide and channel groups cannot supply it."
                    : name === "group_management_power" || name === "group_protection"
                    ? "Management power must reach the protection of both the group and the affected user. The action also needs its own permission. Values range from 0 to 1000. Admins cannot be managed by other users."
                    : numeric
                    ? "The sender's power must be at least the other user's protection. None is 0, Low 25, Normal 50, High 75, Very high 100 and Admin 1000. Advanced lets you enter any whole number from 0 to 1000. Admins are always protected from nonadmins."
                    : field[2];
                if (setting_hint)
                {
                    document.getElementById("groups-description").textContent += " " + setting_hint;
                }
                document.getElementById("groups-reset").disabled = !row_editable;
            };
            label.onclick = describe;
            label.onfocus = describe;
            control.onfocus = describe;
            label_cell.appendChild(label);
            let value_cell = document.createElement("td");
            value_cell.appendChild(control);
            row.append(label_cell, value_cell);
            let saved_flags = group.permissions && group.permissions.flags && group.permissions.flags[name] || {};
            let current_flags = values ? values.flags && values.flags[name] || {} : saved_flags;
            row.append(groups__flag_cell(name, field[1], "negate", current_flags.negate, saved_flags.negate, row_editable));
            if (groups__supports_skip(name))
            {
                row.append(groups__flag_cell(name, field[1], "skip", current_flags.skip, saved_flags.skip, row_editable));
            }
            else
            {
                let unavailable = document.createElement("td");
                unavailable.className = "groups-flag-cell";
                unavailable.textContent = "—";
                unavailable.title = "Skip applies only to permissions that channel groups can change.";
                row.append(unavailable);
            }
            let grant = groups__grant_cell(name, field[1], group.permissions, values, row_editable);
            grant.querySelector("input").onfocus = describe;
            row.append(grant);
            table.appendChild(row);
        }
    }
    table.closest("table").dataset.advanced = String(groups_window.advanced);
    groups__filter_permissions();
    groups__update_save_controls();
}

/**
 * @brief Compares the supplied permission snapshot with a server response.
 * @param expected Draft values, including all its Grant and flag settings.
 * @param actual Server values; omitted Grant defaults to 1000, flags to false.
 */
function groups__permissions_equal(expected, actual)
{
    expected = expected || {};
    actual = actual || {};
    let names = new Set(Object.keys(expected).filter(function(name)
    {
        return name !== "flags" && name !== "grants";
    }));
    for (let name of Object.keys(expected.grants || {}).concat(Object.keys(expected.flags || {})))
    {
        names.add(name);
    }
    for (let name of names)
    {
        if (Object.prototype.hasOwnProperty.call(expected, name))
        {
            let numeric = typeof expected[name] === "number";
            let equal = numeric ? expected[name] === (Number(actual[name]) || 0)
                : groups__action_value(name, expected[name]) === groups__action_value(name, actual[name]);
            if (!equal)
            {
                return false;
            }
        }
        if (groups__required_edit_power(expected, name) !== groups__required_edit_power(actual, name))
        {
            return false;
        }
        for (let flag of ["negate", "skip"])
        {
            let expected_flag = !!(expected.flags && expected.flags[name] && expected.flags[name][flag]);
            let actual_flag = !!(actual.flags && actual.flags[name] && actual.flags[name][flag]);
            if (expected_flag !== actual_flag)
            {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Keeps status text across ordinary membership and permission refreshes.
 */
function groups__set_edit_status(text)
{
    groups_window.edit_status = text;
    document.getElementById("groups-edit-status").textContent = text;
}

/**
 * @brief Stages server permission edits; shared channel controls use their own form handler.
 */
function groups__mark_dirty(event)
{
    if (!event.target.closest("#groups-permission-form") || groups_window.save_pending)
    {
        return;
    }
    let group = g_groups.find(function(entry)
    {
        return entry.group_id === groups_window.selected;
    });
    groups_window.dirty = !!group && (!document.getElementById("groups-permission-form").checkValidity()
        || !groups__permissions_equal(groups__permission_values(), group.permissions));
    groups__set_edit_status(groups_window.dirty ? "Unsaved changes" : "");
    groups__update_save_controls();
}

/**
 * @brief Locks only the pending draft, without changing individual permission authorization.
 */
function groups__update_save_controls()
{
    let pending = !!groups_window.save_pending;
    document.getElementById("groups-permission-form").inert = pending;
    document.getElementById("groups-mode").disabled = pending;
    document.getElementById("groups-list").inert = pending;
    document.getElementById("groups-create").inert = pending;
    document.getElementById("groups-add").disabled = pending || !groups__has_permission("create_groups");
    document.getElementById("groups-delete").disabled = pending || !groups__can_change_group("delete_groups");
    document.getElementById("groups-save").disabled = pending || !g_is_authenticated || !groups__can_manage();
}

/**
 * @brief Ends a pending save and leaves its draft available after a failed confirmation.
 */
function groups__finish_save(status)
{
    if (!groups_window.save_pending)
    {
        return;
    }
    clearTimeout(groups_window.save_timer);
    groups_window.save_timer = null;
    groups_window.save_pending = null;
    groups__set_edit_status(status);
    groups__update_save_controls();
}

/**
 * @brief Sends a draft only when the explicit Save changes button is activated.
 */
function groups__save_changes()
{
    if (groups_window.save_pending || !g_is_authenticated || !groups__can_manage())
    {
        return;
    }
    if (groups_window.dirty && !groups__validate_permissions())
    {
        return;
    }
    let group = g_groups.find(function(entry)
    {
        return entry.group_id === groups_window.selected;
    });
    let values = groups__permission_values();
    let changed = !!group && group.group_id !== 0 && groups_window.dirty
        && !groups__permissions_equal(values, group.permissions);
    if (!changed)
    {
        connection__send_message_object({ message: { type: "save_groups" } });
        groups_window.dirty = false;
        groups__set_edit_status("Save requested.");
        return;
    }
    if (!groups__can_change_group("edit_group_permissions", group) || !groups__has_permission("view_group_permissions"))
    {
        groups__set_edit_status("You no longer have permission to save these changes.");
        return;
    }
    if (!g_is_local_client_admin && Object.values(values.flags || {}).some(function(flags)
    {
        return flags.negate || flags.skip;
    }))
    {
        groups__set_edit_status("Only a full server administrator can save permissions containing Negate or Skip.");
        return;
    }
    for (let name of Object.keys(values))
    {
        if (name === "flags" || name === "grants")
        {
            continue;
        }
        let setting =
        {
            [name]: values[name],
            grants:
            {
                [name]: values.grants[name]
            },
            flags:
            {
                [name]: values.flags && values.flags[name] || {}
            }
        };
        if (!groups__permissions_equal(setting, group.permissions)
            && (!groups__can_edit_setting(group.permissions, name)
                || !g_is_local_client_admin && groups__required_edit_power(values, name) > groups__editing_power()))
        {
            groups__set_edit_status("Your editing power no longer permits every change. Review or reset the changed rows.");
            return;
        }
    }
    groups_window.save_pending =
    {
        group_id: group.group_id,
        permissions: values
    };
    groups_window.save_timer = setTimeout(function()
    {
        groups__finish_save("Save was not confirmed. Your draft is kept; try Save changes again.");
    }, 10000);
    groups__set_edit_status("Saving changes…");
    groups__update_save_controls();
    connection__send_message_object(
    {
        message:
        {
            type: "set_group_permissions",
            group_id: group.group_id,
            permissions: values
        }
    });
}

/**
 * @brief Requests disk persistence only after the server accepts the exact saved draft.
 */
function groups__permissions_saved(group_id, permissions)
{
    let pending = groups_window.save_pending;
    if (!pending || pending.group_id !== group_id || !groups__permissions_equal(pending.permissions, permissions))
    {
        return;
    }
    if (!g_is_authenticated || !groups__can_manage())
    {
        groups__finish_save("Changes were accepted, but you no longer have permission to request saving them.");
        return;
    }
    connection__send_message_object({ message: { type: "save_groups" } });
    groups_window.dirty = false;
    groups__finish_save("Changes accepted. Save requested.");
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
    groups__read_flag_controls(form, values);
    groups__read_grant_controls(form, values);
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
            let changed = control.value !== control.dataset.savedValue || Array.from(row.querySelectorAll("[data-permission-flag]")).some(function(flag)
            {
                return String(flag.checked) !== flag.dataset.savedValue;
            });
            let grant = row.querySelector("[data-permission-grant]");
            changed = changed || !!grant && grant.value !== grant.dataset.savedValue;
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
    document.getElementById("groups-icon-button").disabled = !group || (!admin && !groups__has_permission("upload_icons") && !groups__has_permission("delete_icons"));
    document.getElementById("groups-icon-upload").disabled = !groups__has_permission("upload_icons") || groups_window.icon_reading;
    if (!admin && !groups__has_permission("upload_icons") && !groups__has_permission("delete_icons"))
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
    let can_manage = groups__has_permission("delete_icons");
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
                && (groups__can_change_group("edit_groups", group) || groups__has_permission("upload_icons")))
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
        || (!groups__can_change_group("edit_groups", group) && !groups__has_permission("upload_icons") && !groups__has_permission("delete_icons")))
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
        picker.show();
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
    if (!groups__has_permission("upload_icons") || !files.length || groups_window.icon_reading)
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
        if (generation !== g_worker_connection_generation || !groups__has_permission("upload_icons"))
        {
            break;
        }
        try
        {
            let data = await image_upload__prepare(file, "icon", g_server_policy.icon_max_size);
            if (generation !== g_worker_connection_generation || !groups__has_permission("upload_icons"))
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
    document.getElementById("groups-icon-upload").disabled = !groups__has_permission("upload_icons");
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
function groups__member_identity_hash(client)
{
    if (client.public_key_hash)
    {
        return client.public_key_hash;
    }
    if (!client.public_key)
    {
        return "";
    }
    if (client.groups_identity_source !== client.public_key)
    {
        client.groups_identity_source = client.public_key;
        client.groups_identity_hash = utils__bytesToBase64String(_sha256.create().update(client.public_key).array());
    }
    return client.groups_identity_hash;
}

function groups__member_id(client)
{
    let hash = groups__member_identity_hash(client);
    return hash ? "identity:" + hash : "client:" + client.client_id;
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
    let groups_option = document.getElementById("server-settings-groups-option");
    groups_option.hidden = disabled;
    groups_option.disabled = disabled;
    document.body.classList.toggle("server-groups-disabled", disabled);
    document.getElementById("server-settings-general-temp-channels-row").style.display = g_server_policy.is_permission_system_enabled ? "none" : "";
    document.getElementById("server-settings-general-temp-channels-groups-note").style.display = g_server_policy.is_permission_system_enabled ? "" : "none";
    if (disabled)
    {
        groups__finish_save("Connection closed. Changes were not confirmed saved.");
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
    moderation.channels.clear();
    moderation.clients.clear();
    moderation__refresh();
    groups__schedule_stored_users_refresh();
    if (!G_HAS_DOM)
    {
        return;
    }
    groups__refresh_channel_controls();
    client_status__permissions_changed();
    if (!groups__has_permission("view_group_members"))
    {
        groups_window.identities = [];
    }
    else if (!previously_allowed && document.getElementById("groups-dialog").open)
    {
        connection__send_message_object({ message: { type: "request_identity_list" } });
    }
    if (!groups__has_permission("upload_icons"))
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
        for (let field of ["ip", "country", "last-action", "identity"])
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

/** Adds direct membership submenus without opening the group editor. */
function groups__attach_assignment_menu(menu, client)
{
    if (!menu || !client || client.is_music_bot || !g_server_policy.is_permission_system_enabled)
    {
        return;
    }
    let items = menu.querySelector(".channel-list-context-menu-items");
    let before = items.querySelector('[data-action="7"], [data-action="channel-actions"]');
    for (let scope of ["server", "channel"])
    {
        let item = document.createElement("p");
        item.className = "context-menu-item group-assignment-parent";
        let icon = document.createElementNS("http://www.w3.org/2000/svg", "svg");
        icon.setAttribute("viewBox", "0 0 20 20");
        icon.setAttribute("class", "group-assignment-parent-icon");
        icon.setAttribute("aria-hidden", "true");
        let icon_path = document.createElementNS("http://www.w3.org/2000/svg", "path");
        icon_path.setAttribute("d", scope === "server"
            ? "M7 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6ZM1.5 17v-2a5.5 5.5 0 0 1 11 0v2M13 3a3 3 0 0 1 0 6M15 11a4.5 4.5 0 0 1 3.5 4.4V17"
            : "M6 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6ZM1 17v-2a5 5 0 0 1 8-4M13 3a3 3 0 0 1 0 6M14 11l-1 7M18 11l-1 7M11 13h8M10.5 16h8");
        icon.append(icon_path);
        item.append(icon, document.createTextNode(scope === "server" ? "server groups" : "channel groups"));
        item.dataset.groupScope = scope;
        item.dataset.clientId = client.client_id;
        item.dataset.identity = client.public_key || "";
        item.dataset.channelId = client.channel_id;
        item.tabIndex = 0;
        item.setAttribute("role", "menuitem");
        item.setAttribute("aria-haspopup", "menu");
        item.setAttribute("aria-expanded", "false");
        item.addEventListener("mouseenter", function()
        {
            groups__open_assignment_menu(item);
        });
        item.addEventListener("click", function(event)
        {
            event.stopPropagation();
            groups__open_assignment_menu(item);
        });
        item.addEventListener("keydown", function(event)
        {
            if (["Enter", " ", "ArrowRight"].includes(event.key))
            {
                event.preventDefault();
                event.stopPropagation();
                groups__open_assignment_menu(item);
                document.querySelector('#group-assignment-menu .group-assignment-option[aria-disabled="false"]')?.focus();
            }
            else if (event.key === "Escape")
            {
                UI.delete_contextmenus(true);
            }
        });
        items.insertBefore(item, before);
    }
    for (let item of items.querySelectorAll(".context-menu-item:not(.group-assignment-parent)"))
    {
        item.addEventListener("mouseenter", groups__close_assignment_menu);
    }
    let box = menu.getBoundingClientRect();
    menu.style.left = Math.max(8, Math.min(box.left, window.innerWidth - box.width - 8)) + "px";
    menu.style.top = Math.max(8, Math.min(box.top, window.innerHeight - box.height - 8)) + "px";
    if (!client.is_idle && Number(client.channel_id) >= 0)
    {
        moderation__request_client_access(client);
    }
}

function groups__close_assignment_menu()
{
    document.getElementById("group-assignment-menu")?.remove();
    for (let item of document.querySelectorAll(".group-assignment-parent"))
    {
        item.setAttribute("aria-expanded", "false");
    }
}

/** Resolve the original identity and channel again before sending an assignment. */
function groups__assignment_target(item)
{
    let target = channel_tree__get_client_by_client_id(Number(item.dataset.clientId));
    if (!g_is_authenticated || !g_server_policy.is_permission_system_enabled || !target || target.is_music_bot
        || (target.public_key || "") !== item.dataset.identity)
    {
        return null;
    }
    if (item.dataset.groupScope === "channel" && (target.is_idle
        || Number(target.channel_id) !== Number(item.dataset.channelId)))
    {
        return null;
    }
    return target;
}

function groups__can_assign_channel(target)
{
    return !!target && !target.is_idle && Number(target.channel_id) >= 0
        && moderation__allows(target.channel_id, "assign_channel_groups")
        && (g_is_local_client_admin || (target.client_id !== g_local_client_id && !target.group_ids.includes(0)));
}

function groups__refresh_assignment_menu()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let item = document.querySelector('.group-assignment-parent[data-group-scope][aria-expanded="true"]');
    if (item)
    {
        groups__open_assignment_menu(item);
    }
}

/** Builds the same background and themed rows as the parent context menu. */
function groups__create_submenu(item)
{
    let menu = document.createElement("div");
    menu.id = "group-assignment-menu";
    menu.className = "channel-list-context-menu group-assignment-menu";
    menu.setAttribute("role", "menu");
    menu.setAttribute("aria-label", item.textContent);
    menu.style.setProperty("--context-row-height", item.offsetHeight + "px");
    menu.style.setProperty("--context-menu-width", item.closest(".channel-list-context-menu-items").offsetWidth + "px");
    let background = document.createElement("div");
    background.className = "channel-list-context-menu-background";
    background.setAttribute("aria-hidden", "true");
    let items = document.createElement("div");
    items.className = "channel-list-context-menu-items";
    menu.append(background, items);
    item.setAttribute("aria-expanded", "true");
    menu.addEventListener("keydown", function(event)
    {
        if (["Escape", "ArrowLeft"].includes(event.key))
        {
            event.preventDefault();
            event.stopPropagation();
            groups__close_assignment_menu();
            item.focus();
        }
        else if (["ArrowDown", "ArrowUp"].includes(event.key))
        {
            event.preventDefault();
            event.stopPropagation();
            let options = Array.from(menu.querySelectorAll('.group-assignment-option[aria-disabled="false"]'));
            let index = options.indexOf(document.activeElement);
            options[(index + (event.key === "ArrowDown" ? 1 : -1) + options.length) % options.length]?.focus();
        }
        else if (["Enter", " "].includes(event.key))
        {
            event.preventDefault();
            event.stopPropagation();
            event.target.closest('.group-assignment-option[aria-disabled="false"]')?.click();
        }
    });
    return menu;
}

/** Places every submenu beside its parent and inside the viewport. */
function groups__show_submenu(menu, item)
{
    document.getElementById("contextmenus-container").append(menu);
    let parent = item.closest(".channel-list-context-menu").getBoundingClientRect();
    if (!parent.width || !parent.height)
    {
        parent = item.closest(".channel-list-context-menu-items").getBoundingClientRect();
    }
    let left = parent.right;
    if (left + menu.offsetWidth > window.innerWidth - 8)
    {
        left = parent.left - menu.offsetWidth;
    }
    menu.style.left = Math.max(8, Math.min(left, window.innerWidth - menu.offsetWidth - 8)) + "px";
    menu.style.top = Math.max(8, Math.min(item.getBoundingClientRect().top, window.innerHeight - menu.offsetHeight - 8)) + "px";
}

function groups__open_assignment_menu(item)
{
    groups__close_assignment_menu();
    if (!item.isConnected)
    {
        return;
    }
    let target = groups__assignment_target(item);
    let channel = item.dataset.groupScope === "channel";
    let menu = groups__create_submenu(item);
    let items = menu.querySelector(".channel-list-context-menu-items");
    let entries = channel ? channel_groups.catalog.filter(function(group)
    {
        return Number(group.group_id) !== 0;
    }) : g_groups;
    for (let group of entries)
    {
        let checked = !!target && (channel ? Number(target.channel_group_id || 0) === Number(group.group_id) : target.group_ids.includes(group.group_id));
        let role = checked ? 0 : Number(group.group_id);
        let allowed = channel ? groups__can_assign_channel(target) && moderation__can_assign_role(target, role) : groups__can_change_member(target, group, !checked);
        let button = document.createElement("p");
        button.className = "context-menu-item group-assignment-option";
        button.dataset.groupId = group.group_id;
        moderation__set_menu_access(button, allowed);
        button.setAttribute("role", "menuitemcheckbox");
        button.setAttribute("aria-checked", String(checked));
        button.title = !target ? "This client or channel changed. Open the menu again."
            : !allowed ? "Your permissions do not allow changing this membership."
            : channel ? checked ? "Remove this channel group" : "Assign only in this channel"
            : checked ? "Remove this server group" : "Add this server group";
        let mark = document.createElement("span");
        mark.className = "group-assignment-check";
        mark.textContent = checked ? "\u2713" : "";
        mark.setAttribute("aria-hidden", "true");
        button.append(mark);
        if (channel)
        {
            let image = document.createElement("img");
            let source = channel_groups__icon(group);
            image.className = "groups-icon";
            image.alt = "";
            if (source)
            {
                image.src = source;
            }
            else
            {
                image.hidden = true;
            }
            let label = document.createElement("span");
            label.textContent = group.name;
            button.append(image, label);
        }
        else
        {
            groups__label(button, group);
        }
        let missing_icon = button.querySelector(".groups-icon[hidden]");
        if (missing_icon)
        {
            missing_icon.hidden = false;
            missing_icon.src = "data:image/svg+xml," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20"><path fill="#8d9cab" d="M10 1 18 4v6c0 4-5 7-8 9-3-2-8-5-8-9V4z"/><path fill="none" stroke="white" stroke-width="1.5" d="m6 10 3 3 5-6"/></svg>');
        }
        button.addEventListener("click", function(event)
        {
            event.stopPropagation();
            let current = groups__assignment_target(item);
            if (channel)
            {
                let next_role = current && Number(current.channel_group_id || 0) === Number(group.group_id) ? 0 : Number(group.group_id);
                if (!groups__can_assign_channel(current) || !moderation__can_assign_role(current, next_role))
                {
                    groups__refresh_assignment_menu();
                    return;
                }
                moderation__send("moderate_client",
                {
                    client_id: current.client_id,
                    identity: current.public_key,
                    channel_id: Number(current.channel_id),
                    action: "channel_group",
                    role: next_role
                });
            }
            else
            {
                let add = current && !current.group_ids.includes(group.group_id);
                if (!groups__can_change_member(current, group, add))
                {
                    groups__refresh_assignment_menu();
                    return;
                }
                moderation__send(add ? "add_group_to_client" : "remove_group_from_client",
                {
                    client_id: current.client_id,
                    group_id: group.group_id
                });
            }
            UI.delete_contextmenus(true);
        });
        items.append(button);
    }
    if (!entries.length)
    {
        items.textContent = "No groups available";
    }
    groups__show_submenu(menu, item);
}
