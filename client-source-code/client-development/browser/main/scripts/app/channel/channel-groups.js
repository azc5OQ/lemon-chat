// Channel groups apply only in the channel where the identity was assigned.
var channel_groups =
{
    catalog: [],
    selected: null,
    creating: false,
    previous_selected: null,
    group_search: "",
    panel: null,
    signature: "",
    advanced: false,
    footer: null,
    form: null,
    dirty: false,
    editing: null,
    baseline: "",
    save: null,
    pending: null,
    timer: null,
    notice: "",
    members_panel: null,
    member: null,
    member_group: null,
    adding_member: false,
    member_notice: "",
    member_requests: new Set(),
    member_pending: null,
    member_timer: null
};

var channel_groups_action_fields =
[
    ["bypass_channel_password", "Bypass this channel’s password", "Re-enter this assigned channel without its password, including after reconnecting. Other channel rights still require being inside the channel."],
    ["edit_channel_name", "Edit channel name", "Change the name of the assigned channel."],
    ["edit_channel_description", "Edit channel description", "Change the description of the assigned channel."],
    ["edit_channel_password", "Edit channel password", "Change the password of the assigned channel."],
    ["edit_channel_powers", "Edit required channel powers", "Change the talk, message, picture and streaming requirements of the assigned channel."],
    ["edit_channel_options", "Edit channel options", "Change audio, streaming, capacity and icon settings for the assigned channel."],
    ["delete_channel", "Delete this channel", "Delete the assigned channel. This does not grant permission to delete its child channels."],
    ["assign_channel_groups", "Assign channel groups", "Assign or remove groups for other users in the same channel, within your assignment power."],
    ["kick_from_channel", "Kick users from this channel", "Remove users from this channel by returning them to root. This does not grant moves to other channels."],
    ["mute_clients", "Mute audio in this channel", "Temporarily mute another user's microphone and MP3 audio in this channel."],
    ["stop_client_streams", "Stop streams in this channel", "Stop another user's screen or video stream in this channel."]
];

var channel_groups_power_fields =
[
    ["channel_group_assignment_power", "Channel group assignment power", "Must reach the other user's group protection. You cannot assign rights or powers you do not hold."],
    ["channel_group_protection", "Channel group assignment protection", "Power required to change this user's channel group assignment."],
    ["talk_power", "Talk power", "Compared with this channel's talk requirement for microphone and MP3 audio."],
    ["channel_message_power", "Channel message power", "Compared with this channel's requirement for sending and editing text."],
    ["channel_picture_power", "Channel picture power", "Compared with this channel's requirement for sending pictures."],
    ["stream_power", "Channel streaming power", "Compared with this channel's requirement for screen and video sharing."],
    ["channel_kick_power", "Channel kick power", "Power to remove another user from this channel. It does not increase power to move users between channels."],
    ["channel_kick_protection", "Channel kick protection", "Power required to remove this user from this channel. It does not protect against server kicks or moves."],
    ["channel_moderation_power", "Channel mute and stream stop power", "Power to mute another user's audio or stop their stream in this channel."],
    ["channel_moderation_protection", "Channel mute and stream stop protection", "Power required to mute this user's audio or stop their stream in this channel."]
];

/**
 * @brief Returns the icon shared by every display of this channel group.
 * @param group Channel group definition.
 */
function channel_groups__icon(group)
{
    if (!group)
    {
        return "";
    }
    if (group.icon_id === -2)
    {
        return "data:image/svg+xml;base64," + btoa('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20"><path fill="#648aab" stroke="#324b60" d="M3 3h14v9l-7 6-7-6z"/><path fill="#fff" d="m10 5 1.4 2.9 3.2.5-2.3 2.2.5 3.2-2.8-1.5-2.8 1.5.5-3.2L5.4 8.4l3.2-.5z"/></svg>');
    }
    let icon = group.icon_id >= 0 ? channel_tree__get_icon_by_icon_id(group.icon_id) : null;
    return icon && /^data:image\/(png|jpeg|gif|webp|svg\+xml);base64,/i.test(icon.base64_icon) ? icon.base64_icon : "";
}

/**
 * @brief Finds the group active in the client's current channel.
 * @param client Connected client.
 */
function channel_groups__active(client)
{
    if (!g_server_policy.is_permission_system_enabled || !client || client.channel_id < 0 || client.is_idle)
    {
        return null;
    }
    return channel_groups.catalog.find(function(group)
    {
        return group.group_id === Number(client.channel_group_id);
    }) || null;
}

/**
 * @brief Places channel icons before server icons and labels both types.
 */
function channel_groups__refresh_icons()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    for (let client of g_client_list)
    {
        let holder = document.getElementById("client-groups-" + client.client_id);
        if (!holder)
        {
            continue;
        }
        holder.querySelectorAll(".channel-group-icon").forEach(function(icon)
        {
            icon.remove();
        });
        for (let icon of holder.querySelectorAll("[group-id]"))
        {
            let group = channel_tree__get_group_by_group_id(Number(icon.getAttribute("group-id")));
            icon.title = "Server groups: " + (group ? group.group_name : "");
            icon.setAttribute("aria-label", icon.title);
        }
        let group = channel_groups__active(client);
        let source = channel_groups__icon(group);
        if (source)
        {
            let icon = document.createElement("div");
            icon.className = "single-group channel-group-icon";
            icon.style.backgroundImage = 'url("' + source + '")';
            icon.title = "Channel groups: " + group.name;
            icon.setAttribute("aria-label", icon.title);
            holder.prepend(icon);
        }
    }
    let profile = document.getElementById("current-client-description-groups");
    if (profile && profile.dataset.clientId)
    {
        channel_groups__profile(channel_tree__get_client_by_client_id(Number(profile.dataset.clientId)));
    }
}

/**
 * @brief Separates channel and server memberships in the selected client's details.
 * @param client Selected connected client.
 */
function channel_groups__profile(client)
{
    let holder = document.getElementById("current-client-description-groups");
    holder.replaceChildren();
    holder.dataset.clientId = client ? client.client_id : "";
    if (!client || !g_server_policy.is_permission_system_enabled)
    {
        return;
    }
    let local = channel_groups__active(client);
    let sections = [["Channel groups", local ? [[local.name, channel_groups__icon(local)]] : []],
        ["Server groups", (client.group_ids || []).map(function(id)
        {
            let group = channel_tree__get_group_by_group_id(id);
            let icon = group && group.has_icon ? channel_tree__get_icon_by_icon_id(group.group_linked_icon_id) : null;
            return group ? [group.group_name, icon ? icon.base64_icon : ""] : null;
        }).filter(Boolean)]];
    for (let [title, entries] of sections)
    {
        if (!entries.length)
        {
            continue;
        }
        let heading = document.createElement("strong");
        heading.className = "client-description-group-heading";
        heading.textContent = title;
        holder.append(heading);
        for (let [name, source] of entries)
        {
            let row = document.createElement("div");
            let label = document.createElement("span");
            row.className = "client-description-group-entry";
            if (source)
            {
                let image = document.createElement("img");
                image.className = "groups-icon";
                image.alt = "";
                image.src = source;
                row.append(image);
            }
            label.textContent = name;
            row.append(label);
            holder.append(row);
        }
    }
}

/**
 * @brief Adds channel group management beside server group management.
 */
function channel_groups__setup()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let dialog = document.getElementById("groups-dialog");
    let tabs = document.createElement("nav");
    let panel = document.createElement("div");
    let help = document.getElementById("groups-permission-guide");
    tabs.className = "channel-groups-tabs";
    tabs.setAttribute("aria-label", "Group type");
    panel.id = "channel-groups-panel";
    panel.className = "groups-columns";
    panel.hidden = true;
    channel_groups.panel = panel;
    let footer = document.createElement("div");
    footer.id = "channel-groups-footer";
    footer.className = "groups-footer";
    footer.hidden = true;
    let status = document.createElement("span");
    status.id = "channel-groups-edit-status";
    status.setAttribute("role", "status");
    let discard = moderation__button("Discard changes", function()
    {
        if (channel_groups.pending)
        {
            return;
        }
        channel_groups.dirty = false;
        channel_groups.notice = "";
        channel_groups.signature = "";
        channel_groups__render();
    });
    discard.id = "channel-groups-discard";
    discard.disabled = true;
    let save = moderation__button("Save changes", function()
    {
        if (channel_groups.save)
        {
            channel_groups.save();
        }
    });
    save.id = "channel-groups-save";
    let close = moderation__button("Close", function()
    {
        dialog.close();
    });
    footer.append(status, discard, save, close);
    dialog.append(footer);
    channel_groups.footer = footer;
    for (let [label, page] of [["Server groups", "server"], ["Channel groups", "channel"], ["Permission help", "help"]])
    {
        let button = moderation__button(label, function()
        {
            dialog.querySelector(".groups-columns:not(#channel-groups-panel)").hidden = page !== "server";
            dialog.querySelector(".groups-footer:not(#channel-groups-footer)").hidden = page !== "server";
            footer.hidden = page !== "channel";
            panel.hidden = page !== "channel";
            help.hidden = page !== "help";
            if (page === "channel")
            {
                channel_groups.member_requests.clear();
            }
            document.getElementById("groups-window-title").textContent = label;
            document.getElementById("groups-close").setAttribute("aria-label", "Close " + label.toLowerCase());
            for (let item of tabs.children)
            {
                item.setAttribute("aria-pressed", String(item === button));
            }
            channel_groups__render();
        });
        button.setAttribute("aria-pressed", String(page === "server"));
        tabs.append(button);
    }
    dialog.querySelector(".groups-titlebar").after(tabs);
    tabs.after(panel);
    g_connection_status_listeners.push(function()
    {
        if (!g_is_authenticated)
        {
            channel_groups.catalog = [];
            channel_groups.selected = null;
            channel_groups.creating = false;
            channel_groups.previous_selected = null;
            channel_groups.group_search = "";
            channel_groups.signature = "";
            channel_groups.dirty = false;
            channel_groups.notice = "";
            channel_groups.form = null;
            channel_groups.save = null;
            channel_groups.pending = null;
            channel_groups.members_panel = null;
            channel_groups.member = null;
            channel_groups.member_group = null;
            channel_groups.adding_member = false;
            channel_groups.member_notice = "";
            channel_groups.member_requests.clear();
            channel_groups.member_pending = null;
            clearTimeout(channel_groups.member_timer);
            clearTimeout(channel_groups.timer);
            save.disabled = true;
            discard.disabled = true;
            status.textContent = "";
            panel.replaceChildren();
        }
    });
}

/**
 * @brief Checks the current authority to edit a channel-group definition.
 */
function channel_groups__can_edit(group)
{
    let protected_flags = group && group.permissions && Object.keys(group.permissions.flags || {}).length > 0;
    return groups__has_permission(group ? "edit_channel_groups" : "create_channel_groups")
        && (!group || !!group.permissions) && (g_is_local_client_admin || !protected_flags);
}

/**
 * @brief Marks only this editor's draft; no permission request is sent.
 */
function channel_groups__mark_dirty(event)
{
    if (channel_groups.pending || event && event.target.dataset.permissionView === "true")
    {
        return;
    }
    channel_groups.dirty = true;
    channel_groups.notice = "";
    channel_groups__update_actions();
}

/**
 * @brief Keeps the explicit save action visible and rejects stale drafts.
 */
function channel_groups__update_actions()
{
    let selected = channel_groups.catalog.find(function(group)
    {
        return group.group_id === channel_groups.selected;
    });
    let conflict = channel_groups.baseline !== JSON.stringify(selected || null);
    let allowed = (!!selected || channel_groups.creating) && channel_groups__can_edit(selected);
    let pending = channel_groups.pending != null;
    let save = document.getElementById("channel-groups-save");
    let discard = document.getElementById("channel-groups-discard");
    let status = document.getElementById("channel-groups-edit-status");
    save.textContent = "Save changes";
    save.hidden = channel_groups.creating;
    save.disabled = pending || !allowed || conflict || !selected || !channel_groups.dirty;
    discard.hidden = channel_groups.creating;
    discard.disabled = pending || !channel_groups.dirty;
    let creation = document.getElementById("channel-groups-create");
    if (creation)
    {
        creation.inert = pending;
        document.getElementById("channel-groups-create-submit").disabled = pending || !allowed || conflict;
    }
    if (channel_groups.form)
    {
        channel_groups.form.inert = pending;
        channel_groups.form.setAttribute("aria-busy", String(pending));
    }
    status.textContent = pending ? "Saving changes…"
        : !selected && !channel_groups.creating ? "Select a group."
            : !allowed ? "You cannot edit this group."
            : conflict ? "This group changed on the server. Discard changes to reload it before editing."
                : channel_groups.dirty ? "Unsaved changes"
                    : channel_groups.notice || "Changes apply only when you save.";
}

/**
 * @brief Accepts a channel-group save only after its definition arrives from the server.
 */
function channel_groups__finish_save()
{
    let pending = channel_groups.pending;
    if (!pending)
    {
        return;
    }
    let saved = channel_groups.catalog.find(function(group)
    {
        return (pending.action === "create" ? !pending.existing_ids.has(group.group_id) : group.group_id === pending.group_id)
            && group.name === pending.name && group.icon_id === pending.icon_id
            && (group.permissions ? groups__permissions_equal(pending.permissions, group.permissions) : pending.action === "create");
    });
    if (!saved)
    {
        return;
    }
    clearTimeout(channel_groups.timer);
    channel_groups.timer = null;
    channel_groups.pending = null;
    channel_groups.dirty = false;
    channel_groups.selected = saved.group_id;
    channel_groups.creating = false;
    channel_groups.notice = "Changes saved.";
    channel_groups.signature = "";
}

/**
 * @brief Shows definitions and users without mixing them with server groups.
 */
function channel_groups__render()
{
    let panel = channel_groups.panel;
    if (!G_HAS_DOM || !panel)
    {
        return;
    }
    channel_groups__finish_save();
    if (!channel_groups.creating && !channel_groups.catalog.some(function(group)
    {
        return group.group_id === channel_groups.selected;
    }))
    {
        channel_groups.selected = channel_groups.catalog[0]?.group_id ?? null;
    }
    if (panel.hidden)
    {
        channel_groups__render_member_controls();
        return;
    }
    let signature = JSON.stringify([channel_groups.catalog, channel_groups.selected, channel_groups.creating, groups_window.access,
        g_is_local_client_admin, groups__editing_power()]);
    if (signature === channel_groups.signature)
    {
        channel_groups__render_members();
        return;
    }
    let selected = channel_groups.catalog.find(function(group)
    {
        return group.group_id === channel_groups.selected;
    });
    if ((channel_groups.dirty || channel_groups.pending) && channel_groups.editing === channel_groups.selected)
    {
        channel_groups__render_members();
        channel_groups__update_actions();
        return;
    }
    channel_groups.signature = signature;
    channel_groups.dirty = false;
    channel_groups.editing = channel_groups.selected;
    channel_groups.baseline = JSON.stringify(selected || null);
    let sidebar = document.createElement("aside");
    let list = document.createElement("div");
    let details = document.createElement("section");
    sidebar.className = "groups-sidebar";
    list.className = "channel-groups-list";
    list.setAttribute("aria-label", "Channel groups");
    details.className = "groups-details";
    let label = document.createElement("label");
    label.htmlFor = "channel-groups-search";
    label.textContent = "Channel groups";
    let search = document.createElement("input");
    search.id = "channel-groups-search";
    search.type = "search";
    search.placeholder = "Find a group";
    search.value = channel_groups.group_search;
    search.oninput = function()
    {
        channel_groups.group_search = search.value;
        for (let button of list.children)
        {
            button.hidden = !button.textContent.toLowerCase().includes(search.value.trim().toLowerCase());
        }
    };
    for (let group of channel_groups.catalog)
    {
        let button = moderation__button(group.name, async function()
        {
            if (channel_groups.pending || channel_groups.selected === group.group_id)
            {
                return;
            }
            if (channel_groups.dirty && !await groups__ask("Discard the unsaved changes for this channel group?", "Discard"))
            {
                return;
            }
            channel_groups.dirty = false;
            channel_groups.creating = false;
            channel_groups.selected = group.group_id;
            channel_groups.notice = "";
            channel_groups__render();
        });
        button.dataset.channelGroupId = String(group.group_id);
        button.addEventListener("mouseenter", channel_groups__render_members);
        button.addEventListener("focus", channel_groups__render_members);
        button.setAttribute("aria-pressed", String(selected === group));
        let source = channel_groups__icon(group);
        if (source)
        {
            let image = document.createElement("img");
            image.src = source;
            image.alt = "";
            image.className = "groups-icon";
            button.prepend(image);
        }
        list.append(button);
    }
    search.oninput();
    let actions = document.createElement("div");
    actions.className = "groups-toolbar";
    sidebar.append(label, search, list, actions);
    if (groups__has_permission("create_channel_groups"))
    {
        actions.append(moderation__button("+ Add", async function()
        {
            if (channel_groups.pending || channel_groups.creating || channel_groups.dirty
                && !await groups__ask("Discard the unsaved changes before creating a channel group?", "Discard"))
            {
                return;
            }
            channel_groups.dirty = false;
            channel_groups.previous_selected = channel_groups.selected;
            channel_groups.creating = true;
            channel_groups.selected = null;
            channel_groups.notice = "";
            channel_groups.signature = "";
            channel_groups__render();
            document.getElementById("channel-groups-new-name").focus();
        }));
        actions.lastElementChild.disabled = channel_groups.creating;
    }
    let protected_flags = selected && selected.permissions && Object.keys(selected.permissions.flags || {}).length > 0;
    if (selected && selected.group_id !== 1 && groups__has_permission("delete_channel_groups")
        && (g_is_local_client_admin || !protected_flags))
    {
        actions.append(moderation__button("Delete group", async function()
        {
            if (channel_groups.pending)
            {
                return;
            }
            if (await groups__ask("Delete " + selected.name + " and its channel assignments?", "Delete group"))
            {
                moderation__send("edit_channel_group", { action: "delete", group_id: selected.group_id });
                channel_groups.dirty = false;
            }
        }));
    }
    if (!selected && !channel_groups.creating)
    {
        channel_groups.form = null;
        channel_groups.save = null;
        let empty = document.createElement("p");
        empty.textContent = "Select a group or use + Add to create one.";
        details.append(empty);
        panel.replaceChildren(sidebar, details, channel_groups__member_panel());
        channel_groups__update_actions();
        channel_groups__render_members();
        return;
    }
    let can_edit = channel_groups__can_edit(selected);
    let form = document.createElement("form");
    form.id = "channel-groups-permission-form";
    form.addEventListener("input", channel_groups__mark_dirty);
    form.addEventListener("change", function(event)
    {
        if (event.target.name)
        {
            channel_groups__mark_dirty();
        }
    });
    channel_groups.form = form;
    let title = document.createElement("h2");
    let name = document.createElement("input");
    let icon_id = selected ? selected.icon_id : -1;
    title.textContent = selected ? selected.name : "Initial permissions";
    let scope = document.createElement("p");
    scope.className = "groups-description";
    scope.textContent = "These permissions apply only while the user is in the channel where this group is assigned.";
    name.type = "text";
    name.maxLength = 31;
    name.required = true;
    name.value = selected ? selected.name : "";
    name.setAttribute("aria-label", "Channel group name");
    name.placeholder = "Group name";
    name.disabled = !can_edit;
    name.addEventListener("input", function()
    {
        name.setCustomValidity("");
        if (channel_groups.creating)
        {
            channel_groups__mark_dirty();
        }
    });
    let icon_button = moderation__button("Choose icon…", function()
    {
        channel_groups__choose_icon(icon_id, function(id)
        {
            icon_id = id;
            icon_button.textContent = id === -1 ? "No icon" : id === -2 ? "Default icon" : "Icon " + id;
            channel_groups__mark_dirty();
        });
    });
    icon_button.disabled = !can_edit;
    let view = moderation__select("Permission view", [["simple", "Simple"], ["advanced", "Advanced"]]);
    view.dataset.permissionView = "true";
    view.value = channel_groups.advanced ? "advanced" : "simple";
    let tools = document.createElement("div");
    tools.className = "groups-toolbar";
    tools.append(icon_button, view);
    if (selected)
    {
        form.append(name);
    }
    else
    {
        let creation = document.createElement("div");
        creation.id = "channel-groups-create";
        name.id = "channel-groups-new-name";
        name.placeholder = "New group name";
        name.setAttribute("form", form.id);
        let buttons = document.createElement("div");
        buttons.className = "groups-toolbar";
        let create = moderation__button("Create", function()
        {
            if (channel_groups.save)
            {
                channel_groups.save();
            }
        });
        create.id = "channel-groups-create-submit";
        let cancel = moderation__button("Cancel", function()
        {
            if (channel_groups.pending)
            {
                return;
            }
            channel_groups.creating = false;
            channel_groups.dirty = false;
            channel_groups.selected = channel_groups.previous_selected;
            channel_groups.signature = "";
            channel_groups.notice = "";
            channel_groups__render();
        });
        buttons.append(create, cancel);
        creation.append(name, buttons);
        sidebar.append(creation);
    }
    form.append(tools, scope);
    let table = document.createElement("table");
    table.className = "groups-permission-table";
    table.dataset.advanced = String(channel_groups.advanced);
    let heading = document.createElement("thead");
    let heading_row = document.createElement("tr");
    let value_heading = null;
    for (let label of ["Permission", "Setting", "Negate", "Grant"])
    {
        let cell = document.createElement("th");
        cell.scope = "col";
        cell.textContent = label;
        if (label === "Setting")
        {
            value_heading = cell;
            cell.textContent = channel_groups.advanced ? "Value" : "Setting";
        }
        if (label === "Negate")
        {
            cell.title = "The lowest flagged value wins, unless a server permission has Skip. See Permission help.";
        }
        if (label === "Grant")
        {
            cell.className = "groups-grant-heading";
            cell.title = "Required permission editing power. See Permission help.";
        }
        heading_row.append(cell);
    }
    heading.append(heading_row);
    table.append(heading);
    let values = selected ? selected.permissions || {} : {};
    let controls = [];
    function power_control(key, label, value)
    {
        let control = document.createElement(channel_groups.advanced ? "input" : "select");
        control.name = key;
        control.setAttribute("aria-label", label);
        control.disabled = !can_edit || !!selected && !groups__can_edit_setting(selected.permissions, key);
        control.title = groups__setting_hint(selected ? selected.permissions : {}, key, !control.disabled);
        if (channel_groups.advanced)
        {
            control.type = "number";
            control.min = "0";
            control.max = "1000";
            control.step = "1";
            control.required = true;
        }
        else
        {
            for (let [caption, number] of [["None", 0], ["Low", 25], ["Normal", 50], ["High", 75], ["Very high", 100], ["Admin", 1000]])
            {
                control.append(new Option(caption, String(number)));
            }
            if (![0, 25, 50, 75, 100, 1000].includes(Number(value)))
            {
                control.append(new Option("Custom", String(value)));
            }
        }
        control.value = value;
        return control;
    }
    view.onchange = function()
    {
        for (let control of form.querySelectorAll("input[type='number']"))
        {
            if (!control.reportValidity())
            {
                view.value = channel_groups.advanced ? "advanced" : "simple";
                return;
            }
        }
        channel_groups.advanced = view.value === "advanced";
        table.dataset.advanced = String(channel_groups.advanced);
        value_heading.textContent = channel_groups.advanced ? "Value" : "Setting";
        for (let entry of controls)
        {
            if (entry[2])
            {
                let replacement = power_control(entry[0], entry[1].getAttribute("aria-label"), entry[1].value);
                entry[1].replaceWith(replacement);
                entry[1] = replacement;
            }
        }
    };
    for (let [key, label, description] of channel_groups_action_fields.concat(channel_groups_power_fields))
    {
        let numeric = channel_groups_power_fields.some(function(field)
        {
            return field[0] === key;
        });
        let row = document.createElement("tr");
        let caption = document.createElement("td");
        let cell = document.createElement("td");
        let control = numeric ? power_control(key, label, Number(values[key]) || 0) : moderation__select(label, [["grant", "Enable"], ["deny", "Disable"]]);
        caption.textContent = label;
        caption.title = description;
        control.name = key;
        control.setAttribute("aria-label", label);
        let row_editable = can_edit && (!selected || groups__can_edit_setting(values, key));
        control.disabled = !row_editable;
        let setting_hint = groups__setting_hint(values, key, row_editable);
        caption.title = setting_hint ? setting_hint + " " + description : description;
        row.title = setting_hint;
        if (!numeric)
        {
            control.value = values[key] === "grant" ? "grant" : "deny";
            control.title = "Disable + Negate restricts the action in this channel, unless the server permission has Skip. See Permission help.";
        }
        if (setting_hint)
        {
            control.title = setting_hint + " " + (control.title || description);
        }
        controls.push([key, control, numeric]);
        cell.append(control);
        row.append(caption, cell);
        let flags = values.flags && values.flags[key] || {};
        row.append(groups__flag_cell(key, label, "negate", flags.negate, flags.negate, row_editable));
        row.append(groups__grant_cell(key, label, values, null, can_edit && groups__can_edit_setting(values, key)));
        table.append(row);
    }
    let scroll = document.createElement("div");
    scroll.className = "groups-table-scroll";
    scroll.hidden = !!selected && !selected.permissions;
    scroll.append(table);
    form.append(scroll);
    form.onsubmit = function(event)
    {
        event.preventDefault();
    };
    channel_groups.save = function()
    {
        let current = channel_groups.catalog.find(function(group)
        {
            return group.group_id === channel_groups.selected;
        });
        if (channel_groups.pending || !channel_groups__can_edit(current)
            || channel_groups.baseline !== JSON.stringify(current || null))
        {
            channel_groups__update_actions();
            return;
        }
        let trimmed_name = name.value.trim();
        name.setCustomValidity(!trimmed_name || new TextEncoder().encode(trimmed_name).length > 31
            ? "Use a name between 1 and 31 bytes long." : "");
        if (!can_edit || !form.reportValidity())
        {
            return;
        }
        let permissions = {};
        for (let [key, control, numeric] of controls)
        {
            permissions[key] = numeric ? Number(control.value) : control.value;
        }
        groups__read_flag_controls(form, permissions);
        groups__read_grant_controls(form, permissions);
        if (current)
        {
            for (let [key] of controls)
            {
                let setting =
                {
                    [key]: permissions[key],
                    grants: { [key]: permissions.grants[key] },
                    flags: { [key]: permissions.flags && permissions.flags[key] || {} }
                };
                if (!groups__permissions_equal(setting, current.permissions)
                    && (!groups__can_edit_setting(current.permissions, key)
                        || !g_is_local_client_admin && groups__required_edit_power(permissions, key) > groups__editing_power()))
                {
                    document.getElementById("channel-groups-edit-status").textContent = "Your editing power no longer permits every change. Review or discard the draft.";
                    return;
                }
            }
        }
        let update =
        {
            action: selected ? "update" : "create",
            group_id: selected ? selected.group_id : 0,
            name: trimmed_name,
            icon_id: icon_id,
            permissions: permissions
        };
        let pending = Object.assign({}, update,
        {
            existing_ids: new Set(channel_groups.catalog.map(function(group)
            {
                return group.group_id;
            }))
        });
        channel_groups.pending = pending;
        channel_groups__update_actions();
        moderation__send("edit_channel_group", update);
        channel_groups.timer = setTimeout(function()
        {
            if (channel_groups.pending === pending)
            {
                channel_groups.pending = null;
                channel_groups__update_actions();
                document.getElementById("channel-groups-edit-status").textContent = "Save not confirmed. Your edits are still here; check the connection and try again.";
            }
        }, 10000);
    };
    let note = document.createElement("p");
    note.textContent = "Assignments apply in one channel. They return when the same identity joins that channel again. Saved changes also survive server restarts for permanent channels.";
    details.append(title, note, form);
    panel.replaceChildren(sidebar, details, channel_groups__member_panel());
    channel_groups__update_actions();
    channel_groups__render_members();
}

/**
 * @brief Lists a channel group's visible members in its hover and focus description.
 */
function channel_groups__render_members()
{
    channel_groups__render_member_controls();
    let panel = channel_groups.panel;
    if (!panel)
    {
        return;
    }
    for (let button of panel.querySelectorAll("[data-channel-group-id]"))
    {
        let group_id = Number(button.dataset.channelGroupId);
        let names = [];
        if (!groups__has_permission("view_group_members"))
        {
            button.title = "You cannot view group members.";
            continue;
        }
        for (let client of g_client_list)
        {
            let group = channel_groups__active(client);
            if (!group || group.group_id !== group_id)
            {
                continue;
            }
            let channel = channel_tree__get_channel_by_id(g_channel_list, client.channel_id);
            names.push(client.username + " (" + (channel ? channel.channel_name || channel.name : client.channel_id) + ")");
        }
        button.title = names.length ? names.join("\n") : "No users currently have this group in their channel.";
    }
}

/**
 * @brief Creates the member column shared across channel permission editor refreshes.
 */
function channel_groups__member_panel()
{
    if (channel_groups.members_panel)
    {
        return channel_groups.members_panel;
    }
    let panel = document.createElement("aside");
    panel.className = "groups-members-panel";
    panel.innerHTML = `
        <label id="channel-groups-member-label" for="channel-groups-member-search">Members</label>
        <input type="search" id="channel-groups-member-search" placeholder="Find a user">
        <div id="channel-groups-members" aria-label="Channel group members"></div>
        <div class="groups-toolbar">
            <button type="button" id="channel-groups-member-add">Add</button>
            <button type="button" id="channel-groups-member-remove">Remove</button>
        </div>
        <button type="button" id="channel-groups-member-cancel" hidden>Back to members</button>
        <p id="channel-groups-member-note" role="status"></p>`;
    panel.querySelector("input").oninput = channel_groups__render_member_controls;
    panel.querySelector("#channel-groups-member-add").onclick = function()
    {
        if (channel_groups.adding_member)
        {
            channel_groups__change_membership(true);
            return;
        }
        channel_groups.adding_member = true;
        channel_groups.member = null;
        channel_groups.member_notice = "";
        panel.querySelector("input").value = "";
        channel_groups__render_member_controls();
        panel.querySelector("input").focus();
    };
    panel.querySelector("#channel-groups-member-remove").onclick = function()
    {
        channel_groups__change_membership(false);
    };
    panel.querySelector("#channel-groups-member-cancel").onclick = function()
    {
        channel_groups.adding_member = false;
        channel_groups.member = null;
        channel_groups.member_notice = "";
        panel.querySelector("input").value = "";
        channel_groups__render_member_controls();
    };
    channel_groups.members_panel = panel;
    return panel;
}

/** @brief Binds a selected row to its identity, channel and current assignment. */
function channel_groups__member_key(client)
{
    return JSON.stringify([Number(client.client_id), client.public_key, Number(client.channel_id), Number(client.channel_group_id) || 0]);
}

/** @brief Refreshes the inline member list without replacing the permission draft. */
function channel_groups__render_member_controls()
{
    let panel = channel_groups.members_panel;
    if (!panel || !panel.isConnected)
    {
        return;
    }
    let group_id = channel_groups.selected;
    let group = channel_groups.catalog.find(function(entry)
    {
        return entry.group_id === group_id;
    });
    let search = panel.querySelector("input");
    if (channel_groups.member_group !== group_id)
    {
        channel_groups.member_group = group_id;
        channel_groups.member = null;
        channel_groups.adding_member = false;
        channel_groups.member_notice = "";
        search.value = "";
    }
    let pending = channel_groups.member_pending;
    if (pending)
    {
        let current = moderation__target(pending.client_id, pending.identity, pending.channel_id);
        if (!current || Number(current.channel_group_id || 0) === pending.role)
        {
            clearTimeout(channel_groups.member_timer);
            channel_groups.member_pending = null;
            channel_groups.member_notice = current ? "Membership updated." : "The user left or changed channels.";
            if (current && pending.group_id === group_id)
            {
                channel_groups.adding_member = false;
                channel_groups.member = pending.role ? channel_groups__member_key(current) : null;
            }
        }
    }
    if (channel_groups.panel.hidden)
    {
        return;
    }
    let allowed = g_is_authenticated && !!group && groups__has_permission("view_group_members");
    let adding = channel_groups.adding_member;
    let query = search.value.trim().toLowerCase();
    let container = panel.querySelector("#channel-groups-members");
    let scroll = container.scrollTop;
    let focused = document.activeElement && document.activeElement.dataset.channelGroupMember;
    let selected = null;
    let can_add = false;
    container.replaceChildren();
    for (let client of allowed ? g_client_list : [])
    {
        if (client.is_idle || client.is_music_bot || Number(client.channel_id) < 0 || !client.public_key)
        {
            continue;
        }
        let key = channel_groups__member_key(client);
        if (!channel_groups.member_requests.has(key))
        {
            channel_groups.member_requests.add(key);
            moderation__request_client_access(client);
        }
        let assigned = Number(client.channel_group_id) === group_id;
        can_add ||= !assigned && groups__can_assign_channel(client) && moderation__can_assign_role(client, group_id);
        let channel = channel_tree__get_channel_by_id(g_channel_list, client.channel_id);
        let name = client.username + (Number(client.client_id) === Number(g_local_client_id) ? " (you)" : "")
            + " (" + (channel ? channel.channel_name || channel.name : client.channel_id) + ")";
        if (assigned === adding || !name.toLowerCase().includes(query))
        {
            continue;
        }
        let row = moderation__button("", function()
        {
            channel_groups.member = key;
            channel_groups.member_notice = "";
            moderation__request_client_access(client);
            channel_groups__render_member_controls();
        });
        row.className = "groups-member";
        row.dataset.channelGroupMember = key;
        row.setAttribute("aria-pressed", String(channel_groups.member === key));
        row.title = name;
        let label = document.createElement("span");
        label.className = "groups-member-name";
        label.textContent = name;
        row.append(label);
        container.append(row);
        if (channel_groups.member === key)
        {
            selected = client;
        }
    }
    if (!selected)
    {
        channel_groups.member = null;
    }
    let can_change = allowed && !channel_groups.member_pending && !!selected && groups__can_assign_channel(selected);
    search.disabled = !allowed;
    panel.querySelector("#channel-groups-member-label").textContent = adding ? "Add a member" : "Members";
    panel.querySelector("#channel-groups-member-cancel").hidden = !adding;
    panel.querySelector("#channel-groups-member-add").disabled = !allowed || !!channel_groups.member_pending
        || (adding ? !can_change || !moderation__can_assign_role(selected, group_id) : !can_add);
    panel.querySelector("#channel-groups-member-remove").disabled = adding || !can_change || !moderation__can_assign_role(selected, 0);
    panel.querySelector("#channel-groups-member-note").textContent = !allowed ? ""
        : channel_groups.member_pending ? "Membership change requested."
            : channel_groups.member_notice || (adding ? "Adding replaces the user's group in the listed channel." : "");
    if (!container.childElementCount)
    {
        let empty = document.createElement("p");
        empty.className = "groups-empty";
        empty.textContent = !group ? "Select a saved group."
            : !allowed ? "You do not have permission to view this group's members."
                : query ? "No matching users." : adding ? "No users available to add." : "No members in this group.";
        container.append(empty);
    }
    container.scrollTop = scroll;
    for (let row of container.children)
    {
        if (focused && row.dataset.channelGroupMember === focused)
        {
            row.focus({ preventScroll: true });
        }
    }
}

/** @brief Rechecks the selected identity and authority before changing its channel membership. */
function channel_groups__change_membership(add)
{
    let group_id = channel_groups.selected;
    let client = g_client_list.find(function(entry)
    {
        return channel_groups__member_key(entry) === channel_groups.member;
    });
    let role = add ? group_id : 0;
    if (channel_groups.member_pending || !groups__has_permission("view_group_members")
        || !channel_groups.catalog.some(function(group)
        {
            return group.group_id === group_id;
        }) || !client || !moderation__target(client.client_id, client.public_key, client.channel_id)
        || (Number(client.channel_group_id) === group_id) === add
        || !groups__can_assign_channel(client) || !moderation__can_assign_role(client, role))
    {
        channel_groups__render_member_controls();
        return;
    }
    channel_groups.member_pending =
    {
        client_id: client.client_id, identity: client.public_key,
        channel_id: Number(client.channel_id), role: role, group_id: group_id
    };
    moderation__send("moderate_client",
    {
        client_id: client.client_id, identity: client.public_key,
        channel_id: Number(client.channel_id), action: "channel_group", role: role
    });
    clearTimeout(channel_groups.member_timer);
    channel_groups.member_timer = setTimeout(function()
    {
        channel_groups.member_pending = null;
        channel_groups.member_notice = "No update received. Check the membership before trying again.";
        channel_groups__render_member_controls();
    }, 10000);
    channel_groups__render_member_controls();
}

/**
 * @brief Offers a bounded page of shared icons for a channel group.
 * @param selected Current icon ID.
 * @param accept Called with the chosen icon ID.
 */
function channel_groups__choose_icon(selected, accept)
{
    let body = moderation__open("Channel group icon", false);
    moderation.dialog.dataset.page = "channel-icon";
    let search = document.createElement("input");
    search.type = "search";
    search.placeholder = "Find an icon by ID";
    search.setAttribute("aria-label", search.placeholder);
    let grid = document.createElement("div");
    grid.className = "channel-groups-icon-options";
    let toolbar = document.createElement("div");
    let status = document.createElement("span");
    let page = 0;
    let previous = moderation__button("Previous", function()
    {
        page--;
        render();
    });
    let next = moderation__button("Next", function()
    {
        page++;
        render();
    });
    function render()
    {
        let icons = g_icons.filter(function(icon)
        {
            return String(icon.id).includes(search.value.trim());
        });
        let pages = Math.max(1, Math.ceil(icons.length / 80));
        page = Math.max(0, Math.min(page, pages - 1));
        grid.replaceChildren();
        for (let icon of [{ id: -1 }, { id: -2 }].concat(icons.slice(page * 80, page * 80 + 80)))
        {
            let button = moderation__button("", function()
            {
                accept(icon.id);
                moderation.dialog.close();
            });
            button.title = icon.id === -1 ? "No icon" : icon.id === -2 ? "Default icon" : "Icon " + icon.id;
            button.setAttribute("aria-label", button.title);
            button.setAttribute("aria-pressed", String(icon.id === selected));
            let source = channel_groups__icon({ icon_id: icon.id });
            if (source)
            {
                let image = document.createElement("img");
                image.src = source;
                image.alt = "";
                button.append(image);
            }
            else
            {
                button.textContent = "None";
            }
            grid.append(button);
        }
        previous.disabled = page === 0;
        next.disabled = page + 1 === pages;
        status.textContent = "Page " + (page + 1) + " of " + pages;
    }
    search.oninput = function()
    {
        page = 0;
        render();
    };
    toolbar.append(previous, status, next);
    body.append(search, grid, toolbar);
    render();
}
