// the protocol layer, split out of main.js (same script element, same closure).
// server_msg = every server->client message handler; client_msg = the builders
// for client->server requests. wire format details live here, not in the UI

var server_msg = {
    // fills g_tags from the server snapshot, renders the settings tag table, and paints
    // tag chips onto every visible client row (clients in hidden channels are skipped)
    process_tag_list_from_server: function(msg)
    {
        UI.wire_settings_delete_delegation();

        for (let i = 0; i < msg.message.tags.length; i++)
        {
            let tag = msg.message.tags[i];
            g_tags.push(tag);

            let icon = tag.has_icon ? get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

            let base64_icon = "";

            if (icon != null)
            {
                base64_icon = icon.base64_icon;
            }

            // the admin tag cannot be deleted, but its row still needs the column or the
            // cells above it shift out of line with the header
            let tag_delete_button_html = (tag.tag_id != 0)
                ? "<button class=\"settings-entry-delete-button\" title=\"delete tag\">✕</button>"
                : "<span class=\"settings-entry-delete-spacer\"></span>";
            let html_to_append = "<div class=\"server-settings-tag-entry\" data-tag-id=\""+tag.tag_id+"\">\n\
                                    <p class=\"tag-settings-entry-p\">"+tag.tag_id+"</p>\n\
                                    <p class=\"tag-settings-entry-p\">"+tag.tag_name+"</p>\n\
                                    <p class=\"tag-settings-entry-p\">"+tag.tag_linked_icon_id+"</p>\n\
                                    <div class=\"tag-settings-entry-img\" style=\"background-image: url("+base64_icon+");\"></div>\n\
                                    "+tag_delete_button_html+"\n\
                                </div>";

            document.getElementById("server-settings-tab-tags-container").insertAdjacentHTML("beforeend", html_to_append);
        }

        // add g_tags for clients in UI

        for (let i = 0; i < g_client_list.length; i++)
        {
            console.log("process_tag_list_from_server | processing client: " + g_client_list[i].username);
            for (let y = 0; y < g_client_list[i].tag_ids.length; y++)
            {
                console.log("process_tag_list_from_server | processing tag id : " + g_client_list[i].tag_ids[y]);

                let tag = get_tag_by_tag_id(g_client_list[i].tag_ids[y]);

                if (tag == null)
                {
                    console.log("tag is null");
                    continue;
                }

                let target_element = document.getElementById("client-tags-" + g_client_list[i].client_id);

                // clients in a hidden channel have no row, and throwing here aborted the loop
                // so every client after them lost their g_tags too
                if (target_element == null)
                {
                    continue;
                }

                const node = document.createElement("div");
                node.className = "single-tag";
                node.setAttribute("tag-id", tag.tag_id);

                let icon = tag.has_icon ? get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

                if (icon != null)
                {
                    node.style.backgroundImage = "url("+icon.base64_icon+")";
                }

                target_element.appendChild(node);
            }
        }
    },
    // renders the admin identity-management list: one row per stored identity - abbreviated
    // hash (full hash in title + on the controls' data-hash), last-seen username, online dot,
    // its g_tags as chips each with a remove ✕, an "add tag" dropdown, and a delete-identity ✕
    process_identity_list_from_server: function(msg)
    {
        let list = document.getElementById("server-settings-identities-list");
        if (list == null) { return; }

        let identities = (msg && msg.message && msg.message.identities) ? msg.message.identities : [];

        let tag_display_name = function(tag_id)
        {
            if (tag_id == 0) { return "admin"; }
            let tag = get_tag_by_tag_id(tag_id);
            return (tag != null && tag.tag_name) ? tag.tag_name : ("#" + tag_id);
        };

        let html = "";
        for (let i = 0; i < identities.length; i++)
        {
            let identity = identities[i];
            let hash = identity.public_key_hash != null ? identity.public_key_hash : "";
            let hash_attr = sanitize_string(hash);
            let short_hash = hash.length > 18 ? (hash.substring(0, 18) + "…") : hash;
            let username = (identity.username != null && identity.username.length > 0) ? identity.username : "—";
            let tag_ids = identity.tag_ids != null ? identity.tag_ids : [];

            // g_tags as removable chips
            let chips = "";
            for (let t = 0; t < tag_ids.length; t++)
            {
                chips += "<span class=\"identity-tag-chip\">" + sanitize_string(tag_display_name(tag_ids[t]))
                        + "<span class=\"identity-tag-remove\" data-identity-hash=\"" + hash_attr + "\" data-tag-id=\"" + tag_ids[t] + "\" title=\"remove this tag from the identity\">✕</span></span>";
            }

            // "add tag" dropdown: server g_tags this identity does not already hold (admin tag excluded)
            let add_options = "<option value=\"\">+ tag</option>";
            for (let t = 0; t < g_tags.length; t++)
            {
                if (g_tags[t].tag_id == 0) { continue; }
                if (tag_ids.indexOf(g_tags[t].tag_id) != -1) { continue; }
                add_options += "<option value=\"" + g_tags[t].tag_id + "\">" + sanitize_string(g_tags[t].tag_name) + "</option>";
            }
            let add_select = "<select class=\"identity-tag-add\" data-identity-hash=\"" + hash_attr + "\" title=\"give this identity a tag\">" + add_options + "</select>";

            let online_dot = identity.is_online == true
                ? "<span title='online' style='color:#4caf50;'>●</span>"
                : "<span title='offline' style='color:#888;'>○</span>";

            // an identity is REGISTERED exactly when an admin gave it an alias - that is what
            // unlocks the offline people list and offline messages, so it is worth stating
            // plainly rather than leaving the admin to infer it from an empty cell
            let alias = (identity.alias != null) ? identity.alias : "";
            let is_registered = (alias.length > 0);

            let registered_cell = is_registered
                ? "<span title='has an alias: may list offline people and receive offline messages' style='color:#4caf50;'>yes</span>"
                : "<span title='no alias: not registered on this server' style='opacity:0.6;'>no</span>";

            // edit in place: type a name and press enter (or the ✓) to register, ✕ clears it
            let alias_cell = "<input class=\"identity-alias-input\" data-identity-hash=\"" + hash_attr + "\" value=\"" + sanitize_string(alias) + "\" placeholder=\"no alias\" title=\"admin-registered display name; empty means not registered\">"
                            + "<button class=\"identity-alias-set-button\" data-identity-hash=\"" + hash_attr + "\" title=\"save this alias\">✓</button>"
                            + (is_registered ? "<button class=\"identity-alias-clear-button\" data-identity-hash=\"" + hash_attr + "\" title=\"clear the alias (unregisters this identity)\">✕</button>" : "");

            html += "<div class=\"identity-entry\">"
                    + "<p class=\"identity-entry-p\" title=\"" + hash_attr + "\">" + sanitize_string(short_hash) + "</p>"
                    + "<p class=\"identity-entry-p\" title=\"" + sanitize_string(username) + "\">" + sanitize_string(username) + "</p>"
                    + "<p class=\"identity-entry-p identity-entry-p-online\">" + online_dot + "</p>"
                    + "<p class=\"identity-entry-p identity-entry-p-registered\">" + registered_cell + "</p>"
                    + "<div class=\"identity-entry-alias\">" + alias_cell + "</div>"
                    + "<div class=\"identity-entry-tags\">" + chips + add_select + "</div>"
                    + "<button class=\"identity-delete-button\" data-identity-hash=\"" + hash_attr + "\" title=\"delete this identity and strip all its tags\">✕</button>"
                    + "</div>";
        }

        if (identities.length == 0)
        {
            html = "<p style=\"font-size:11px; opacity:0.6;\">no stored identities yet</p>";
        }

        list.innerHTML = html;
    },
    // pushes each icon into g_icons, appends its settings entry, then repaints the
    // channel icons that were rendered empty before the icons arrived
    process_icon_list_from_server: function(msg)
    {
        UI.wire_settings_delete_delegation();

        for (let i = 0; i < msg.message.icons.length; i++)
        {
            let icon = {
                id: msg.message.icons[i].icon_id,
                base64_icon: msg.message.icons[i].base64_icon
            }

            g_icons.push(icon);

            let html_to_append = "<div class='server-settings-icon-entry' data-icon-id="+icon.id+"><img class='img-uploaded-icon' src="+icon.base64_icon+"></img><button class='settings-entry-delete-button' title='delete icon'>✕</button></div>";

            document.getElementById("server-settings-tab-icons-container").insertAdjacentHTML("beforeend", html_to_append);
        }

        // g_channel_list arrives BEFORE this icon_list, so channel rows were rendered with empty icon
        // boxes; now that the g_icons exist, paint them
        UI.refresh_all_channel_icons();
    },
    // one-shot: builds the whole channel tree HTML from the server snapshot, fills
    // g_channel_list, wires click/touch handlers, sets g_is_channel_list_retrieved
    process_channel_list_from_server: function(msg)
    {
        if (g_is_channel_list_retrieved == true)
        {
            custom_log("channel_list message received more than once. Server is doing something weird");
            return;
        }

        g_is_channel_list_retrieved = true;
        document.getElementById("channel-list-container").innerHTML = "";
        var root_channels = get_channels_by_channel_parent_id(msg.message.channels, g_ROOT_LEVEL_PARENT_SENTINEL);

        let processed_channels = [];
        let current_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_SENTINEL;
        let previous_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_SENTINEL;
        let channel_tree_build_stall_counter = 0;

        while (processed_channels.length != msg.message.channels.length)
        {
            let processed_count_before_iteration = processed_channels.length;

            if (current_parent_channel_id_to_find == previous_parent_channel_id_to_find)
            {
                current_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_SENTINEL;
            }
            previous_parent_channel_id_to_find = current_parent_channel_id_to_find;

            var child_channels = get_channels_by_channel_parent_id(msg.message.channels, current_parent_channel_id_to_find);

            for (let a = 0; a < child_channels.length; a++)
            {
                // first, check if current child channel is present in HTML. That is done by checking its presence in g_channel_list array.
                // if not, add current channel to g_channel_list and append channel to HTML

                let is_channel_added_to_html = get_channel_by_id(g_channel_list, child_channels[a].channel_id);

                if (is_channel_added_to_html == null)
                {
                    child_channels[a].is_channel_directly_collapsed = false;
                    g_channel_list.push(child_channels[a]);

                    let indentation_level = get_indentation_level(child_channels[a].channel_id, msg.message.channels);

                    let html_to_append = "";
                    let html_to_append_audio_disabled_class = (child_channels[a].is_audio_enabled == false) ? "single-channel-is-audio-disabled" : "";
                    let html_to_append_is_using_password_class = (child_channels[a].is_using_password == true) ? "single-channel-is-using-password" : "";
                    let html_to_append_is_temp_class = (child_channels[a].is_temp_channel == true) ? "single-channel-is-temp" : "";

                    if (child_channels[a].is_using_password == true)
                    {
                        console.log("channel " + child_channels[a].name + " is using password !!");

                        html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + child_channels[a].channel_id + "\" data-channel-parent-id=\"" + child_channels[a].parent_channel_id + "\">\n\
                                            <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                            <div class=\"single-channel-collapse-button\">\n\
                                            </div>\n\
                                            <p class='single-channel-name-p "+html_to_append_is_using_password_class+" "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + child_channels[a].channel_id + "\">" + sanitize_string(child_channels[a].name) + "</p>\n\
                                            <div class=\"single-channel-icon\" data-channel-icon-for=\"" + child_channels[a].channel_id + "\"></div>\n\
                                            <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + child_channels[a].channel_id + "\"></p>\n\
                                            <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                        </div>";
                    }
                    else
                    {
                        console.log("channel " + child_channels[a].name + " is not using password !!");

                        html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + child_channels[a].channel_id + "\" data-channel-parent-id=\"" + child_channels[a].parent_channel_id + "\">\n\
                                                <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                                <div class=\"single-channel-collapse-button\">\n\
                                                </div>\n\
                                                <p class='single-channel-name-p "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + child_channels[a].channel_id + "\" >" + sanitize_string(child_channels[a].name) + "</p>\n\
                                                <div class=\"single-channel-icon\" data-channel-icon-for=\"" + child_channels[a].channel_id + "\"></div>\n\
                                            <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + child_channels[a].channel_id + "\"></p>\n\
                                                <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                            </div>";
                    }

                    document.getElementById("channel-list-container").insertAdjacentHTML("beforeend", html_to_append);
                }

                // next, check if channel is already processed. If it is, check if for loop is at its end.
                // If for loop is at its end, and current channel is processed, that means all child channels of a parent channel were processed and that they have no more inner child channels in them

                let is_channel_already_processed = processed_channels.indexOf(child_channels[a].channel_id) != -1;

                if (is_channel_already_processed == true)
                {
                    if ((a + 1) == child_channels.length)
                    {
                        if (current_parent_channel_id_to_find == g_ROOT_LEVEL_PARENT_SENTINEL)
                        {
                            continue;
                        }

                        // all child channels in parent channel are processed.\
                        // add parent channel of child channels in this loop to processed_channels

                        processed_channels.push(current_parent_channel_id_to_find);
                    }
                }
                else
                {
                    // if channel is not processed, find out if the current channel in loop has any child channels. If it has, change current_parent_channel_id_to_find
                    // if not, push channel to processed_channel list

                    var children_of_child_channel = get_channels_by_channel_parent_id(msg.message.channels, child_channels[a].channel_id);
                    if (children_of_child_channel.length > 0)
                    {
                        current_parent_channel_id_to_find = child_channels[a].channel_id;
                        break;
                    }

                    processed_channels.push(child_channels[a].channel_id);
                }
            }

            // self-recovery guard: a healthy pass always processes at least one channel.
            // if an entire pass made no progress, the channel data is inconsistent
            // (missing root, orphaned child, or a cycle) - stop instead of freezing the UI.
            if (processed_channels.length == processed_count_before_iteration)
            {
                channel_tree_build_stall_counter++;
                if (channel_tree_build_stall_counter > msg.message.channels.length + 1)
                {
                    console.error("channel tree builder made no progress - received channel data is invalid. " + (msg.message.channels.length - processed_channels.length) + " channel(s) could not be placed; aborting render");
                    break;
                }
            }
            else
            {
                channel_tree_build_stall_counter = 0;
            }
        }

        let elements = document.getElementsByClassName('single-channel');
        // let elements = document.querySelector(".single-channel:not(.idle-channel)");

        // handle click events on channels differently on touch devices
        if (g_is_client_running_under_touch_device)
        {
            for (let i = 0; i < elements.length; i++)
            {
                let local_touch_press_timer = null;

                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                elements[i].addEventListener("touchstart", (event) => {

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    g_is_pressing = true;

                    local_touch_press_timer = window.setTimeout( () => {
                        if (g_is_pressing)
                        {
                            UI.single_channel_onmousedown(event, true, clientX, clientY, currentTarget);
                            g_is_pressing = false;
                        }
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {
                    g_is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].addEventListener("touchcancel", (event) => {
                    g_is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }
        else
        {
            for (let i = 0; i < elements.length; i++)
            {
                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                elements[i].addEventListener("mousedown", UI.single_channel_onmousedown);
                let selected_channel_id1 = parseInt(elements[i].getAttribute("data-channel-id"));
                console.log("selected_channel_id1 => " + selected_channel_id1)
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("dblclick", UI.single_channel_doubleclick_join);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("click", UI.single_channel_onclick);
                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }

        // recompute chevron (leaf) visibility after the channel tree changed
        UI.refresh_all_channel_fullness();
    },
    // hides the song marquee on that client's row; for the local client also sets
    // g_stop_song_stream_message_received so the song stops being sent to the server
    process_stop_song_stream_from_server: function(msg)
    {
        // start / stop song stream messages server pupose of handling marquee animation where text moves
        // microphone changes in gui are handles elsewhere

        // disable css marquee effect

        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + msg.message.client_id + '"]');
        if (element != null)
        {
            element.style.display = "none";
            document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = "";
        }
        else
        {
            console.log("could not find element");
        }

        // set g_stop_song_stream_message_received to true, so song is not sent anymore to server, this variable is then read elsewhere
        // this only applies to local client

        if (msg.message.client_id == local_client_id)
        {
            g_stop_song_stream_message_received = true;
        }
    },
    // adds the new client to g_client_list (+ id map), renders his row with handlers, promotes
    // his offline chat, re-keys the root channel if we maintain it, and requests his avatar
    process_client_connect_from_server: function(msg)
    {
        let username = sanitize_string(msg.message.username);
        let client_id = msg.message.client_id;

        if (get_client_by_client_id(local_client_id).channel_id == msg.message.channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_entered_your_channel.play();
            }
        }
        let single_client = {
            client_id: null,
            username: null,
            alias: null,
            public_key: null,
            channel_id: null,
            audio_state: null,
            tag_ids: null,
            is_clients_channel_hidden: null,
            country_iso_code: null,
            is_idle: null,
            is_ignored_by_local_client: null,
            is_muted_by_local_client: null,
            unread_count: 0,
            is_music_bot: null
        };

        single_client.client_id = parseInt(client_id);
        single_client.username = username;
        single_client.alias = (msg.message.alias != null) ? msg.message.alias : "";
        single_client.public_key = msg.message.public_key;
        single_client.channel_id = msg.message.channel_id;
        single_client.tag_ids = (msg.message.tag_ids != null) ? msg.message.tag_ids : [];
        single_client.audio_state = msg.message.audio_state;
        single_client.is_clients_channel_hidden = false;
        single_client.country_iso_code = msg.message.country_iso_code;
        single_client.is_idle = false;
        single_client.is_ignored_by_local_client = false;
        single_client.is_muted_by_local_client = false;
        single_client.unread_count = 0;
        single_client.is_music_bot = msg.message.is_music_bot;

        g_client_list.push(single_client);
        g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);

        // they were offline and we have a conversation open with them: turn it into their
        // live private chat, keeping the history. without this the offline context and the
        // live one are two separate boxes for the same person - replies land in the live
        // one while you are still looking at (and typing into) the offline one.
        //
        // the merge is state, so it happens here; only the markup move is left to the ui.
        // as one UI.* call it was a no-op with no dom, and a reconnecting contact kept a
        // stale offline-keyed context forever
        let promotion = promote_offline_chat_context_state(single_client);
        UI.promote_offline_chat_context_render(promotion, single_client);

        html_to_append = generate_html_for_single_client(single_client, false);

        get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

        let elements = document.getElementsByClassName('connected-client');

        // local user uses diferent handler
        for (let i = 0; i < elements.length; i++)
        {

            if (g_is_client_running_under_touch_device)
            {

                let local_touch_press_timer = null; // for touch devices

                elements[i].addEventListener("touchstart", (event) => {

                    g_is_long_press = false;

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    local_touch_press_timer = window.setTimeout( () => {

                        g_is_long_press = true;
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {

                    clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                    const currentTarget = event.currentTarget;
                    const clientY = event.changedTouches[0].pageY;
                    const clientX = event.changedTouches[0].pageX;

                    if (g_is_long_press == false)
                    {
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                    }
                });

            }
            else
            {
                elements[i].addEventListener("mousedown", UI.connected_user_onmousedown);
            }
            // elements[i].style.backgroundColor = "";
        }

        // html got re-written, assign event handler to html element again
        // used addEventListener("focusout",function), object.onfocusout = function() did not work in chrome

        { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout); } }

        // if local_user is maintainer of root channel, he will sent new keys for root channel
        // if not, client will wait for channel keys
        // current_channel_keys must be set to null at client_connect if local client is at current_channnel_id

        if (current_channel_id == 0)
        {
            current_channel_keys = null;
            console.log("setting current keys to null")
        }

        let index = get_channel_index_in_array_by_channel_id(g_channel_list, current_channel_id);

        // if we are maintainers of root channel, send new keys

        if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id == local_client_id && current_channel_id == 0)
        {
            create_and_send_new_channel_keys();
        }
        else
        {
            console.log("waiting for keys from maintainer");
        }

        // add tag ids

        console.log("process_tag_list_from_server | processing client: " +  single_client.username);
        for (let y = 0; y < single_client.tag_ids.length; y++)
        {
            console.log("process_tag_list_from_server | processing tag id : " + single_client.tag_ids[y]);

            let tag = get_tag_by_tag_id(single_client.tag_ids[y]);

            if (tag == null)
            {
                console.log("tag is null");
                continue;
            }

            let target_element = document.getElementById("client-tags-" + single_client.client_id);

            const node = document.createElement("div");
            node.className = "single-tag";
            node.setAttribute("tag-id", tag.tag_id);

            // has_icon is the only thing saying an icon was assigned, and id 0 resolves to a real
            // icon, so an unguarded lookup paints one on g_tags that have none
            let icon = (tag.has_icon == true) ? get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

            if (icon != null)
            {
                node.style.backgroundImage = "url("+icon.base64_icon+")";
            }

            target_element.appendChild(node);
        }

        UI.refresh_all_channel_fullness();

        // a new client joined: pull their avatar (if any) so it shows on their row
        request_single_avatar(client_id);
    },
    // removes the client from the DOM, g_client_list and the id map (swap-with-last), keeps
    // aliased clients in g_offline_client_list, and drops his private chat context if open
    process_client_disconnect_from_server: function(msg)
    {
        clear_typing_from_client(msg.message.client_id);  // gone, so nothing is being typed

        let disconnecting_client = get_client_by_client_id(msg.message.client_id);
        let local_client = get_client_by_client_id(local_client_id);

        // a disconnect can arrive for a client we never fully registered locally (e.g. one still
        // mid key-exchange). the map guard further down returns cleanly in that case, but this
        // early sound-effect check must not dereference an undefined client first
        if (disconnecting_client != null && local_client != null && disconnecting_client.channel_id == local_client.channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_disconnected_from_your_channel.play();
            }
        }

        // an aliased client is registered with the server, so he lingers as an offline
        // contact instead of vanishing. the stored-clients snapshot only arrives on
        // connect - anyone who registered since then is missing from it, so keep the
        // local copy fresh ourselves. a later reconnect of his drops the offline row
        // again (the member list pairs by alias).
        if (disconnecting_client != null && typeof disconnecting_client.alias === "string" && disconnecting_client.alias.length > 0)
        {
            let already_stored = false;
            for (let i = 0; i < g_offline_client_list.length; i++)
            {
                if (g_offline_client_list[i].alias.toLowerCase() == disconnecting_client.alias.toLowerCase())
                {
                    already_stored = true;
                    break;
                }
            }

            if (already_stored == false)
            {
                g_offline_client_list.push({
                    alias: disconnecting_client.alias,
                    base64_avatar: (typeof disconnecting_client.base64_avatar === "string") ? disconnecting_client.base64_avatar : "",
                    tag_ids: (disconnecting_client.tag_ids != null) ? disconnecting_client.tag_ids : [],
                    last_seen: Math.floor(new Date().valueOf() / 1000) // we watched them leave: that IS their last-seen
                });
            }
        }

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        if (document.getElementById('chat-context-pm-' + msg.message.client_id) != null)
        {
            document.getElementById('chat-context-pm-' + msg.message.client_id).remove();
        }

        let chat_context_index_to_remove = get_chat_context_index_by_chat_context_id("chat-context-pm-" + msg.message.client_id);

        if (chat_context_index_to_remove != -1)
        {
            g_chat_context_array.splice(chat_context_index_to_remove, 1);
        }

        // if used chat context was deleted one, switch to current channel
        if (g_current_chat_context_id == "chat-context-pm-" + msg.message.client_id)
        {
            console.log("switching to main channel? ");
            g_current_chat_context_id = "chat-context-channel-" + current_channel_id;
        clear_channel_unread_count(current_channel_id); // opened it, so it is read
            document.getElementById("chat-context-channel-" + current_channel_id).style.display = "block";
            g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_alias = ""; // back on a channel: no offline target
            UI.schedule_member_list_sync(); // active ring moves back to the channel circle
        }

        // let client_index_to_remove = get_client_index_in_array_by_client_id(parseInt(msg.message.client_id));
        // g_client_list.splice(client_index_to_remove, 1);

        let index = g_map_client_id_to_array_index.get(parseInt(msg.message.client_id));
        if (index === undefined || index == -1)
        {
            // disconnect for a client that was never in our local list (e.g. one that dropped
            // mid key-exchange). we skip the array removal, but say so instead of silently returning
            console.warn("process_client_disconnect_from_server: no local entry for client_id " + msg.message.client_id + " (index " + index + "); skipping client_list removal");
            return;
        }

        let lastIndex = g_client_list.length - 1;
        let lastClient = g_client_list[lastIndex];

        // move last client into removed spot
        g_client_list[index] = lastClient;
        g_map_client_id_to_array_index.set(lastClient.client_id, index);

        // remove last
        g_client_list.pop();
        g_map_client_id_to_array_index.delete(parseInt(msg.message.client_id));

        UI.refresh_all_channel_fullness();
    },
    // the two routes to the native accept/decline screen: the Android object when a
    // webview exists, the bridge listener when node runs headless
    process_call_from_server: function(msg)
    {
        let client = get_client_by_client_id(msg.message.caller_client_id);

        if (client == null)
        {
            return;
        }

        if (g_is_running_in_android_webview == true)
        {
            Android.JavaExportStartCall(client.username, client.channel_id);
            return;
        }

        if (g_node_incoming_call_listener != null)
        {
            g_node_incoming_call_listener(client.username, client.channel_id);
            return;
        }

        custom_alert("incoming call from " + sanitize_string(client.username));
    },
    // moves a client out of idle: updates his channel_id/is_idle, re-renders his row in the
    // target channel with handlers, plays the join sound and may resume the always-on mic
    process_client_coming_back_from_idle_mode_from_server: function(msg)
    {
        g_client_list[get_client_index_in_array_by_client_id(msg.message.client_id)].channel_id = msg.message.channel_id;
        g_client_list[get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = false;

        // the local client returned from idle: sync the channel globals - a stale
        // current_channel_id made the maintainer_id message (and with it the channel
        // keys) be silently discarded when returning straight into a call
        if (msg.message.client_id == local_client_id)
        {
            current_channel_id = msg.message.channel_id;
            current_channel_keys = null;

            // the server answered, so we are allowed to go idle again
            clear_come_from_idle_in_flight();
        }

        // someone else returned into OUR channel and we are its maintainer: rotate the
        // keys so the returner can communicate - same as on a normal channel join
        if (msg.message.client_id != local_client_id && msg.message.channel_id == current_channel_id)
        {
            let key_channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, current_channel_id);

            if (key_channel_index != -1 && g_channel_list[key_channel_index].has_maintainer && g_channel_list[key_channel_index].maintainer_id == local_client_id)
            {
                console.log("client " + msg.message.client_id + " returned from idle into current channel, rotating channel keys as maintainer");
                create_and_send_new_channel_keys();
            }
        }

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        let single_client = g_client_list[get_client_index_in_array_by_client_id(msg.message.client_id)];

        if (local_client_id != single_client.client_id)
        {
            html_to_append = generate_html_for_single_client(single_client, false);
        }
        else
        {
            html_to_append = generate_html_for_single_client(single_client, true);
        }

        get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

        let element = document.querySelector('.connected-client[data-connected-client-id="'+msg.message.client_id+'"]');

        if (g_is_client_running_under_touch_device)
        {
            let local_touch_press_timer = null; // for touch devices

            element.addEventListener("touchstart", (event) => {

                g_is_long_press = false;

                // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                // by the time setTimeout runs, event object (or at least part of it) is lost,
                // things from event object must be imediatelly stored in temp variable in this case, for later use

                const currentTarget = event.currentTarget;
                const clientY = event.touches[0].clientY;
                const clientX = event.touches[0].clientX;

                local_touch_press_timer = window.setTimeout( () => {

                    g_is_long_press = true;
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                },
                600, event);

            });

            element.addEventListener("touchend", (event) => {

                clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                const currentTarget = event.currentTarget;
                const clientY = event.changedTouches[0].pageY;
                const clientX = event.changedTouches[0].pageX;

                if (g_is_long_press == false)
                {
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                }
            });
        }
        else
        {
            element.addEventListener("mousedown", UI.connected_user_onmousedown);
        }

        // play sound in case the client that came back from idle mode joined channel of local client
        let local_client_channel_id = get_client_by_client_id(local_client_id).channel_id;

        if (local_client_channel_id == msg.message.channel_id && local_client_id != msg.message.client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_joined_your_channel.play();
                g_sound_effects.call.pause();
                g_sound_effects.currentTime = 0;
            }
        }

        // continuous transmission no longer auto-starts on join - it is tap-toggled

    },
    // applies the server's SDP offer to g_peer_connection_with_server, then creates
    // an answer, sets it as local description and sends it back
    process_sdp_offer_from_server: function(msg)
    {
        // voice lives in the webview only. node sees this frame too (it owns the socket)
        // but never built a peer connection, so it just forwards and ignores
        if (g_peer_connection_with_server == null)
        {
            return;
        }

        console.log(msg);

        let description = JSON.parse(msg.message.value); // value is OBJECT, contains sdp and type

        console.log("description -> ", description);

        g_peer_connection_with_server.setRemoteDescription(description)
            .then(function ()
            {
                console.log("sending answer...");
                g_peer_connection_with_server.createAnswer()
                    .then(function (answer)
                    {
                        g_peer_connection_with_server.setLocalDescription(answer);
                        send_sdp_answer_to_server(answer);
                    })
                    .catch(function (error)
                    {
                        console.log(`Failed to create session description: ${error.toString()}`);
                    });
            })
            .catch(function (error)
            {
                console.log(`Failed to set session description: ${error.toString()}`);
            });
    },
    // removes the deleted channel from g_channel_list and the DOM; plays the
    // delete sound when the local client requested it
    process_channel_delete_from_server: function(msg)
    {
        console.log("channel deleted: channel_id=" + msg.message.channel_id);
        if (msg.message.channel_deletor_id == local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_deleted.play();
            }
        }

        let channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        if (channel_index != -1)
        {
            g_channel_list.splice(channel_index, 1);
            document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').remove();
            UI.refresh_all_channel_fullness();
        }
    },
    // feeds a server-sent ICE candidate into g_peer_connection_with_server
    process_ice_candidate_from_server: function(msg)
    {
        // same as the sdp offer: node has no peer connection, and in the browser a
        // candidate can still arrive just after a teardown nulled it
        if (g_peer_connection_with_server == null)
        {
            return;
        }

        console.log("got ice candidate from server");
        g_peer_connection_with_server.addIceCandidate(msg.message.value);
    },
    // appends the server broadcast text to the root channel chat context,
    // rebuilds the trailing chat-spacer and scrolls the chat to the bottom
    process_server_info_broadcast_from_server: function(msg)
    {
        let index = get_chat_context_index_by_chat_context_id("chat-context-channel-0");
        console.log("process_server_info_broadcast_from_server chat context index -> " + index);
        g_chat_context_array[index].last_known_message_sender_username = "";
        let html_to_append = "<div class=\"single-server-message\">" + sanitize_string(msg.message.value) + " " + new Date().toLocaleTimeString() + "</div>";
        document.getElementById("chat-context-channel-0").insertAdjacentHTML("beforeend", html_to_append);
        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append1 = "<div class=\"chat-spacer\"></div>";
        document.getElementById("chat-context-channel-0").insertAdjacentHTML("beforeend", html_to_append1);
        document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;
    },
    // pairs our just-sent message/picture with its server id (stored as an attribute, enabling
    // the right-click menu) and records our own public key as that message's author
    process_server_chat_message_id_for_local_message_id_from_server: function(msg)
    {
        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = g_rsa_public_key_string;

        let element = document.querySelector('.local-single-chat-message-content-p[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

        if (element != null)
        {
            element.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
            element.addEventListener("mousedown", UI.single_chat_message_onrightclick);
        }
        else
        {
            let element = document.querySelector('.local-client-chat-picture-img[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

            if (element != null)
            {
                element.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
                element.addEventListener("mousedown", UI.single_chat_message_onrightclick);
            }
        }
    },
    // applies the edited channel fields to g_channel_list and its row (name text plus the
    // password/audio-disabled classes); plays the edited sound if the local client edited it
    process_channel_edit_from_server: function(msg)
    {
        console.log("channel edited: channel_id=" + msg.message.channel_id);
        if (msg.message.channel_editor_id == local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_edited.play();
            }
        }

        let index = get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2].innerHTML = sanitize_string(msg.message.channel_name);
        g_channel_list[index].description = msg.message.channel_description;
        g_channel_list[index].is_using_password = msg.message.is_using_password;
        g_channel_list[index].name = msg.message.channel_name;
        g_channel_list[index].is_audio_enabled = msg.message.is_audio_enabled;
        g_channel_list[index].is_client_limit_active = msg.message.is_client_limit_active;
        g_channel_list[index].max_client_count = msg.message.max_client_count;
        UI.refresh_all_channel_fullness();

        if (msg.message.is_using_password == true)
        {
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2];
            if (element1.classList.contains("single-channel-is-using-password") == false)
            {
                element1.classList.add("single-channel-is-using-password");
            }
        }
        else
        {
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2];
            if (element1.classList.contains("single-channel-is-using-password") == true)
            {
                element1.classList.remove("single-channel-is-using-password");
            }
        }

        if (msg.message.is_audio_enabled == false)
        {
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').lastElementChild;

            console.log("audio is disabled, adding class single-channel-is-audio-disabled");

            if (element1.classList.contains("single-channel-is-audio-disabled") == false)
            {
                element1.classList.add("single-channel-is-audio-disabled");
                console.log("audio is disabled, class added");

            }
        }
        else
        {
            console.log(msg.message);
            let element1 = document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').lastElementChild;
            console.log("audio is enabled, removing class single-channel-is-audio-disabled");

            if (element1.classList.contains("single-channel-is-audio-disabled") == true)
            {
                element1.classList.remove("single-channel-is-audio-disabled");
                console.log("audio is enabled, class removed");
            }
        }
    },
    // fills and shows the client-info popup (username, connected time, ip, country,
    // last action, identity); the server only replies to admins
    process_client_info_from_server: function(msg)
    {
        // admin-only client info popup (the server only replies to admins)
        let client = get_client_by_client_id(msg.message.client_id);
        let username = (client != null && client.username != null) ? client.username : ("client " + msg.message.client_id);

        let total_seconds = parseInt(msg.message.connected_seconds);
        if (isNaN(total_seconds)) { total_seconds = 0; }
        let days = Math.floor(total_seconds / 86400);
        let hours = Math.floor((total_seconds % 86400) / 3600);
        let minutes = Math.floor((total_seconds % 3600) / 60);

        let country = (msg.message.country_iso_code != null && msg.message.country_iso_code.length > 0) ? msg.message.country_iso_code : "unknown";

        document.getElementById("client-info-username").innerText = username;
        document.getElementById("client-info-connected").innerText = days + "d " + hours + "h " + minutes + "m";
        document.getElementById("client-info-ip").innerText = msg.message.ip_address;
        document.getElementById("client-info-country").innerText = country;
        document.getElementById("client-info-last-action").innerText = parseInt(msg.message.last_action_seconds_ago) + "s ago";
        document.getElementById("client-info-identity").innerText = msg.message.identity;

        document.getElementById("client-info-container").style.display = "block";
    },

    // pushes the new channel into g_channel_list, inserts its row after the parent's subtree,
    // rewires all channel handlers; plays the created sound if the local client created it
    process_channel_create_from_server: function(msg)
    {

        console.log(msg);

        if (msg.message.channel_creator_id == local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_created.play();
            }
        }

        let new_channel = {
            channel_id: msg.message.channel_id,
            parent_channel_id: msg.message.parent_channel_id,
            name: msg.message.name,
            description: msg.message.description,
            maintainer_id: msg.message.maintainer_id,
            is_root_channel: msg.message.is_root_channel,
            has_maintainer: msg.message.has_maintainer,
            is_using_password: msg.message.is_using_password,
            is_audio_enabled: msg.message.is_audio_enabled,
            is_temp_channel: msg.message.is_temp_channel,
            is_client_limit_active: msg.message.is_client_limit_active,
            max_client_count: msg.message.max_client_count,
            has_channel_icon: msg.message.has_channel_icon,
            channel_icon_id: msg.message.channel_icon_id,
            is_channel_directly_collapsed: false,
        };

        g_channel_list.push(new_channel);
        let indentation_level = get_indentation_level(new_channel.channel_id, g_channel_list);

        let html_to_append = "";
        let html_to_append_audio_disabled_class = (new_channel.is_audio_enabled == false) ? "single-channel-is-audio-disabled" : "";
        let html_to_append_is_using_password_class = (new_channel.is_using_password == true) ? "single-channel-is-using-password" : "";
        let html_to_append_is_temp_class = (new_channel.is_temp_channel == true) ? "single-channel-is-temp" : "";

        if (msg.message.is_using_password == true)
        {
            html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + new_channel.channel_id + "\" data-channel-parent-id=\"" + new_channel.parent_channel_id + "\">\n\
                                        <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                        <div class=\"single-channel-collapse-button\">\n\
                                        </div>\n\
                                        <p class='single-channel-name-p "+html_to_append_is_using_password_class+" "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + new_channel.channel_id + "\">" + new_channel.name + "</p>\n\
                                        <div class=\"single-channel-icon\" data-channel-icon-for=\"" + new_channel.channel_id + "\"></div>\n\
                                        <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + new_channel.channel_id + "\"></p>\n\
                                        <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                    </div>";
        }
        else
        {
            html_to_append = "<div class=\"single-channel\" data-channel-id=\"" + new_channel.channel_id + "\" data-channel-parent-id=\"" + new_channel.parent_channel_id + "\">\n\
                                        <div class=\"padding-div\" style=\"padding-left: " + indentation_level * 20 + "px;\"></div>\n\
                                        <div class=\"single-channel-collapse-button\">\n\
                                        </div>\n\
                                        <p class='single-channel-name-p "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + new_channel.channel_id + "\" >" + new_channel.name + "</p>\n\
                                        <div class=\"single-channel-icon\" data-channel-icon-for=\"" + new_channel.channel_id + "\"></div>\n\
                                        <p class=\"single-channel-unread-number unread-empty\" data-channel-unread-for=\"" + new_channel.channel_id + "\"></p>\n\
                                        <div class="+html_to_append_audio_disabled_class+"></div>\n\
                                    </div>";
        }

        // first find out if there are any clients in channel where sub channel is about to be added
        // goal is to append channel after last client connected to the parrent channel, if there is some client present
        // so gui is consistent

        // append the new subchannel at the end of the parent's whole subtree (after the parent's clients
        // and any already-existing subchannels), so siblings keep creation order instead of the new one
        // being prepended right after the parent header
        let subtree_anchor = get_channel_subtree_last_element(new_channel.parent_channel_id);
        subtree_anchor.insertAdjacentHTML("afterend", html_to_append);

        // paint the new row's icon (a fresh channel has none yet, but this also covers the case
        // where it was created carrying one, and keeps the icon box consistent)
        UI.refresh_channel_icon(new_channel);

        let elements = document.getElementsByClassName('single-channel');
        // let elements = document.querySelector(".single-channel:not(.idle-channel)");

        // handle UI differently on touch devices..
        if (g_is_client_running_under_touch_device)
        {
            for (let i = 0; i < elements.length; i++)
            {

                // skip the special idle channel

                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                let local_touch_press_timer = null;

                elements[i].addEventListener("touchstart", (event) => {

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    g_is_pressing = true;

                    local_touch_press_timer = window.setTimeout( () => {
                        if (g_is_pressing)
                        {
                            UI.single_channel_onmousedown(event, true, clientX, clientY, currentTarget);
                            g_is_pressing = false;
                        }
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {
                    g_is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].addEventListener("touchcancel", (event) => {
                    g_is_pressing = false;
                    clearTimeout(local_touch_press_timer);
                });

                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }
        else
        {
            for (let i = 0; i < elements.length; i++)
            {
                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                elements[i].addEventListener("mousedown", UI.single_channel_onmousedown);
                let selected_channel_id1 = parseInt(elements[i].getAttribute("data-channel-id"));
                console.log("selected_channel_id1 => " + selected_channel_id1)
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("dblclick", UI.single_channel_doubleclick_join);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("click", UI.single_channel_onclick);
                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }

        // recompute chevron (leaf) visibility after the channel tree changed
        UI.refresh_all_channel_fullness();
    },
    // appends a placeholder (loading gif) picture message to the channel chat; the real
    // image arrives later and is matched by picture_id. ignored senders are skipped
    process_channel_chat_picture_metadata_from_server: function(msg)
    {
        let client_index = get_client_index_in_array_by_client_id(msg.message.sender_id);

        if (client_index == -1)
        {
            return;
        }

        if (get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        let sender_username = g_client_list[client_index].username;
        let index = get_chat_context_index_by_chat_context_id("chat-context-channel-" + current_channel_id);
        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;

        g_chat_context_array[index].last_known_message_sender_username = sender_username;

        let elements_count = document.getElementsByClassName('chat-spacer').length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName('chat-spacer')[0].remove();
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + generate_message_sender_html(msg.message.sender_id, sanitize_string(sender_username)) + "\n\
                                        </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <img class='chat-picture-img chat-picture-img-default' data-single-chat-message-server-message-id='"+ msg.message.picture_id + "' id=\"chat-picture-img-" + msg.message.picture_id + "\" src=\"" + g_loading_gif + "\"></img>\n\
                                    </div>\n\
                                </div>";

        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
    },
    // private-chat version: creates the pm chat context if missing, plays the received
    // sound, appends the placeholder img and bumps the sender's unread badge
    process_direct_chat_picture_metadata_from_server: function(msg)
    {
        // if direct message is of type direct_chat_message, do not display the message if client is ignored
        if (get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        let received_direct_message_chat_context_id = 'chat-context-pm-' + msg.message.sender_id;

        // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
        // context exists ties state creation to rendering, which breaks with no dom present
        let is_chat_context_existing = false;

        for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
        {
            if (g_chat_context_array[context_index].chat_context_id == received_direct_message_chat_context_id)
            {
                is_chat_context_existing = true;
                break;
            }
        }

        if (g_are_sound_effects_enabled)
        {
            g_sound_effects.message_received.play();
        }

        if (is_chat_context_existing == false)
        {
            console.log("element not found");

            let html_to_append = "<div class=\"chat-context\" id=\"" + received_direct_message_chat_context_id + "\">\n\
                                            <div class=\"single-server-message\">now talking to user: " + sanitize_string(get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                                <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                                <div class=\"single-server-message\">his public key: " + sanitize_string(get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
                                            <div class=\"single-server-message\"></div>\n\
                                        </div>";

            document.getElementById("chat-context-container").insertAdjacentHTML("beforeend", html_to_append);
            document.getElementById(received_direct_message_chat_context_id).style.display = "none";

            let single_chat_context = {
                type: "user",
                chat_context_id: received_direct_message_chat_context_id,
                last_known_message_sender_username: ""
            };

            g_chat_context_array.push(single_chat_context);
        }

        let chat_context_index = get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);
        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + generate_message_sender_html(msg.message.sender_id, sanitize_string(msg.message.sender_username)) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\"><p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            <img class='chat-picture-img-default chat-picture-img' data-single-chat-message-server-message-id='"+ msg.message.picture_id + "' id=\"chat-picture-img-" + msg.message.picture_id + "\" src=\"" + g_loading_gif + "\"></img>\n\
                                        </div>\n\
                                    </div>\n\
                                </div>";

        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append1 = "<div class=\"chat-spacer\"></div>";
        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append1);

        if (g_current_chat_context_id != received_direct_message_chat_context_id)
        {
            increment_unread_count(msg.message.sender_id);
            render_unread_badge(msg.message.sender_id, true);
        }
        g_chat_context_array[chat_context_index].last_known_message_sender_username = msg.message.sender_username;
    },
    // records the announced maintainer on the channel after validating he exists in our
    // channel; we then either send fresh keys (we are him) or arm the keys-wait timer
    process_channel_maintainer_id_from_server: function(msg)
    {
        console.log(msg);

        // we received some maintainer id from server and we are expected to set it
        // but not so fast, dont let server to trick us...
        // first, check if maintainer really exists at our side and if he really is present in current channel at our end

        let client_index = get_client_index_in_array_by_client_id(msg.message.maintainer_id);

        if (client_index == -1)
        {
            console.log("%c process_channel_maintainer_id_from_server: client with id (" + msg.message.maintainer_id + ") does not exist", "color: red");
            return;
        }

        if (g_client_list[client_index].channel_id != current_channel_id)
        {
            console.log("%c process_channel_maintainer_id_from_server: client with id (" + msg.message.maintainer_id + ") not in current channel", "color: red");
            return;
        }

        let index = get_channel_index_in_array_by_channel_id(g_channel_list, current_channel_id);

        // unguarded -1 crashed the whole handler mid-dispatch on device
        if (index == -1)
        {
            console.warn("maintainer message for unknown channel " + current_channel_id + ", ignoring");
            return;
        }

        g_channel_list[index].maintainer_id = msg.message.maintainer_id;
        g_channel_list[index].has_maintainer = msg.message.has_maintainer;
        if (msg.message.has_maintainer && (local_client_id == msg.message.maintainer_id) && (current_channel_id == msg.message.channel_id))
        {
            // reuse the already-validated index; a fresh unguarded lookup crashed here
            console.log("local user is maintainer of channel '" + g_channel_list[index].name + "'");

            // local user is the key SENDER now, there is nothing to wait for
            cancel_maintainer_keys_wait_timer();

            create_and_send_new_channel_keys();
        }
        else if ((current_channel_id == msg.message.channel_id) && (local_client_id != msg.message.maintainer_id))
        {
            console.log("%c received channel_maintainer_id " + msg.message.maintainer_id + " for channel: " + msg.message.channel_id, "color: blue");

            // keys from this maintainer are expected shortly; if none arrive within the
            // timeout, the timer votes for a maintainer reset (and keeps re-voting)
            arm_maintainer_keys_wait_timer();
        }
        else
        {
            console.log("%c process_channel_maintainer_id_from_server: unknown", "color: blue");
        }
    },
    // renders a received channel chat message (font size clamped 12-30, grouped under the
    // previous sender); skips our own echo and ignored senders, plays the received sound
    process_channel_chat_message_from_server: function(msg)
    {
        clear_typing_from_client(msg.message.sender_id);  // the message arrived, he is done

        if (msg.message.sender_id == local_client_id)
        {
            return;
        }

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = get_public_key_by_client_id(msg.message.sender_id);

        if (get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        if (g_are_sound_effects_enabled == true)
        {
            g_sound_effects.message_received.play();
        }

        let chat_message_username = sanitize_string(msg.message.sender_username);
        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let chat_context_index = get_chat_context_index_by_chat_context_id(receiving_chat_context_id);

        // badge the channel unless it is the one on screen
        increment_channel_unread_count(msg.message.channel_id);

        let received_channel_chat_message_object = JSON.parse(msg.message.decrypted_value);
        received_channel_chat_message_object.value = sanitize_string(received_channel_chat_message_object.value);
        let font_size1 = received_channel_chat_message_object.font_size;

        if (custom_typeof(font_size1) != "number")
        {
            console.log("custom_typeof(font_size1) != number");
            console.log("font_size1" + font_size1);
            font_size1 = 12;
        }

        if (font_size1 < 12)
        {
            font_size1 = 12;
        }

        if (font_size1 > 30)
        {
            font_size1 = 30;
        }

        font_size1 = font_size1 + "px";

        if (g_chat_context_array[chat_context_index].last_known_message_sender_username == chat_message_username)
        {
            let chat_message = "<p class='single-chat-message-content-p "+sanitize_string(received_channel_chat_message_object.font)+"' data-single-chat-message-server-message-id='" + msg.message.server_chat_message_id + "' style='color: "+sanitize_string(received_channel_chat_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_channel_chat_message_object.value + "</p>";
            let last_child_index = document.getElementById(receiving_chat_context_id).getElementsByClassName("single-chat-message").length - 1;
            document.getElementById(receiving_chat_context_id).getElementsByClassName("single-chat-message")[last_child_index].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", chat_message);
        }
        else
        {
            let html_to_append = "<div class=\"single-chat-message\">\n\
                                            <div class=\"single-message-content\">\n\
                                                <div class=\"single-chat-message-sender-username-container\">\n\
                                                    " + generate_message_sender_html(msg.message.sender_id, chat_message_username) + "\n\
                                                </div>\n\
                                                <div class=\"single-chat-message-sender-time\">\n\
                                                    <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                                </div>\n\
                                                <div class=\"single-chat-message-content\">\n\
                                                    <p class='single-chat-message-content-p "+sanitize_string(received_channel_chat_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+sanitize_string(received_channel_chat_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_channel_chat_message_object.value + "</p>\n\
                                                </div>\n\
                                            </div>\n\
                                        </div>";

            let elements_count = document.getElementById(receiving_chat_context_id).getElementsByClassName("chat-spacer").length;

            for (let i = 0; i < elements_count; i++)
            {
                document.getElementById(receiving_chat_context_id).getElementsByClassName("chat-spacer")[0].remove();
            }

            document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
            document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");
            document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;
        }

        document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;

        if (g_local_username == chat_message_username)
        {
            document.getElementById("chat-input-container-text-input").value = "";
        }

        g_chat_context_array[chat_context_index].last_known_message_sender_username = chat_message_username;
    },
    // a message somebody left for us while we were offline, handed over on connect. the
    // sender may or may not be connected now, so it is addressed by their ALIAS: the chat
    // context is keyed by alias too, and merges with the live one if they are here.
    process_offline_chat_message_from_server: function(msg)
    {
        let sender_alias = (typeof msg.message.sender_alias === "string") ? msg.message.sender_alias : "";
        let received_object = null;

        try { received_object = JSON.parse(msg.message.some_json.value); }
        catch (e) { console.warn("offline message payload was not readable:", e.message); return; }

        let message_text = sanitize_string(received_object.value);
        let when_text = (typeof msg.message.queued_unix_seconds === "number" && msg.message.queued_unix_seconds > 0)
            ? UI.format_time_ago(msg.message.queued_unix_seconds)
            : "while you were away";

        // if that person is connected right now, their live private chat is the natural
        // home for this; otherwise it lands in an alias-keyed context of its own
        let sender_client = null;
        for (let i = 0; i < g_client_list.length; i++)
        {
            if (g_client_list[i] != null && g_client_list[i].alias == sender_alias && sender_alias.length > 0)
            {
                sender_client = g_client_list[i];
                break;
            }
        }

        // raw alias in the id so it matches what open_offline_chat_context builds; the
        // alias only ever reaches the DOM as text through sanitize_string below
        let context_id = (sender_client != null) ? ("chat-context-pm-" + sender_client.client_id) : ("chat-context-offline-" + sender_alias);

        // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
        // context exists ties state creation to rendering, which breaks with no dom present
        let is_chat_context_existing = false;

        for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
        {
            if (g_chat_context_array[context_index].chat_context_id == context_id)
            {
                is_chat_context_existing = true;
                break;
            }
        }

        if (is_chat_context_existing == false)
        {
            let html_to_append = "<div class=\"chat-context\" id=\"" + context_id + "\" style=\"display: none;\">\n\
                                    <div class=\"single-server-message\">now talking to user: " + sanitize_string(sender_alias) + "</div>\n\
                                </div>";
            document.getElementById('chat-context-container').insertAdjacentHTML("beforeend", html_to_append);

            g_chat_context_array.push({
                type: "user",
                chat_context_id: context_id,
                last_known_message_sender_username: ""
            });

            // a pill so the message is reachable; themes that hide pills (simpledark,
            // bluebell) surface it through the people strip instead
            // raw alias in the id, matching the context id and what the promotion looks up
            // when this person connects; sanitize_string is only for text that is displayed
            let selector_id = (sender_client != null) ? ("user-" + sender_client.client_id) : ("offline-" + sender_alias);
            if (document.querySelector('[data-chat-context-selector-id="' + selector_id + '"]') == null)
            {
                let selector_html = "<div class=\"chat-context-selector\" data-chat-context-selector-type=\"user\" data-chat-context-selector-id=\"" + selector_id + "\">\n\
                                        <div class=\"p-container\"><p>" + sanitize_string(sender_alias) + "</p></div>\n\
                                        <div class=\"remove-chat-context-selector\" data-chat-context-remove-selector-type=\"user\" data-chat-context-remove-selector-id=\"" + selector_id + "\"></div>\n\
                                    </div>";
                document.getElementById("chat-context-selectors-container").insertAdjacentHTML("beforeend", selector_html);

                // freshly inserted markup carries no handlers - rebind them all, the same
                // way every other place that adds a pill does
                for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
                {
                    document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
                }
                for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
                {
                    document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
                }
            }
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                <div class=\"single-message-content\">\n\
                                    <div class=\"single-chat-message-sender-username-container\">\n\
                                        " + generate_message_sender_html((sender_client != null) ? sender_client.client_id : null, sanitize_string(sender_alias)) + "\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + when_text + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        <p class='single-chat-message-content-p'>" + message_text + "</p>\n\
                                    </div>\n\
                                </div>\n\
                            </div>";

        document.getElementById(context_id).insertAdjacentHTML("beforeend", html_to_append);

        // unread marker on that person, exactly like a live private message
        if (sender_client != null)
        {
            increment_unread_count(sender_client.client_id);
            render_unread_badge(sender_client.client_id, true);
        }

        UI.schedule_member_list_sync();
    },

    // two payload types: renders a private chat message (creating the pm context and unread
    // badge), or installs current_channel_keys if they come from the announced maintainer
    process_direct_chat_message_from_server: function(msg)
    {
        clear_typing_from_client(msg.message.sender_id);  // the message arrived, he is done

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = get_public_key_by_client_id(msg.message.sender_id);

        if (msg.message.some_json.type == "direct_chat_message")
        {
            // if direct message is of type direct_chat_message, do not display the message if client is ignored
            if (get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
            {
                return;
            }

            let decrypted_text = msg.message.some_json.value;

            let received_direct_message_object = JSON.parse(decrypted_text);
            received_direct_message_object.value = sanitize_string(received_direct_message_object.value);

            let font_size1 = received_direct_message_object.font_size;

            if (custom_typeof(font_size1) != "number")
            {
                font_size1 = 12;
            }

            if (font_size1 < 12)
            {
                font_size1 = 12;
            }

            if (font_size1 > 30)
            {
                font_size1 = 30;
            }

            font_size1 = font_size1 + "px";

            // log(msg.message.sender_username + ' -> '+ decrypted_text);
            let received_direct_message_chat_context_id = "chat-context-pm-" + msg.message.sender_id;

            // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
            // context exists ties state creation to rendering, which breaks with no dom present
            let is_chat_context_existing = false;

            for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
            {
                if (g_chat_context_array[context_index].chat_context_id == received_direct_message_chat_context_id)
                {
                    is_chat_context_existing = true;
                    break;
                }
            }

            if (is_chat_context_existing == false)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.message_received.play();
                }

                let html_to_append = "<div class=\"chat-context\" id=\"" + received_direct_message_chat_context_id + "\">\n\
                                        <div class=\"single-server-message\">now talking to user:  " + sanitize_string(get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                        <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                        <div class=\"single-server-message\">his public key: " + sanitize_string(get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
                                        <div class=\"single-server-message\"></div>\n\
                                    </div>"

                document.getElementById('chat-context-container').insertAdjacentHTML("beforeend", html_to_append);
                document.getElementById(received_direct_message_chat_context_id).style.display = "none";

                let single_chat_context = {
                    type: "user",
                    chat_context_id: received_direct_message_chat_context_id,
                    last_known_message_sender_username: ""
                };

                g_chat_context_array.push(single_chat_context);
            }

            let chat_context_index = get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);

            if (g_chat_context_array[chat_context_index].last_known_message_sender_username == msg.message.sender_username)
            {
                let chat_message = "<p class='single-chat-message-content-p "+sanitize_string(received_direct_message_object.font)+"' data-single-chat-message-server-message-id='" + msg.message.server_chat_message_id + "' style='color: "+sanitize_string(received_direct_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_direct_message_object.value + "</p>";
                let last_child_index = document.getElementById(received_direct_message_chat_context_id).getElementsByClassName("single-chat-message").length - 1;
                let exists = document.getElementById(received_direct_message_chat_context_id).getElementsByClassName("single-chat-message") != "undefined";
                document.getElementById(received_direct_message_chat_context_id).getElementsByClassName("single-chat-message")[last_child_index].getElementsByClassName("single-chat-message-content")[0].insertAdjacentHTML("beforeend", chat_message);
                // hides the badge without clearing the count, which is what this did before
                render_unread_badge(msg.message.sender_id, false);
            }
            else
            {
                let html_to_append = "<div class=\"single-chat-message\">\n\
                                        <div class=\"single-message-content\">\n\
                                            <div class=\"single-chat-message-sender-username-container\">\n\
                                                " + generate_message_sender_html(msg.message.sender_id, sanitize_string(msg.message.sender_username)) + "\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-sender-time\">\n\
                                                <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-content\">\n\
                                                <p class='single-chat-message-content-p "+sanitize_string(received_direct_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+sanitize_string(received_direct_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_direct_message_object.value + "</p>\n\
                                            </div>\n\
                                        </div>\n\
                                    </div>";

                document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
                let elements_count = document.getElementsByClassName("chat-spacer").length;

                for (let i = 0; i < elements_count; i++)
                {
                    document.getElementsByClassName("chat-spacer")[0].remove();
                }

                let html_to_append1 = "<div class=\"chat-spacer\"></div>";
                document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append1);
            }
            if (g_current_chat_context_id != received_direct_message_chat_context_id)
            {
                increment_unread_count(msg.message.sender_id);
                remember_message_awaiting_receipt(msg.message.sender_id, msg.message.server_chat_message_id);
                render_unread_badge(msg.message.sender_id, true);
            }
            else
            {
                // the conversation is open. send only if the screen is actually on, otherwise owe it
                if (is_the_user_actually_looking() == true)
                {
                    send_seen_receipt_for_message(msg.message.sender_id, msg.message.server_chat_message_id);
                }
                else
                {
                    remember_message_awaiting_receipt(msg.message.sender_id, msg.message.server_chat_message_id);
                }
            }
            g_chat_context_array[chat_context_index].last_known_message_sender_username = msg.message.sender_username;
        }
        // the other side read a private message of ours. the server routed this like any
        // other direct message and never saw what it says
        else if (msg.message.some_json.type == "message_seen")
        {
            // the eye is off, so the receipt is dropped without touching the message
            if (g_show_seen_indicator == false)
            {
                return;
            }

            mark_message_as_seen(msg.message.sender_id);
        }
        else if (msg.message.some_json.type == "channel_keys_from_maintainer")
        {
            let index = get_channel_index_in_array_by_channel_id(g_channel_list, current_channel_id);

            // unguarded -1 crashed exactly here on device and the keys were never set
            if (index == -1)
            {
                console.warn("channel keys arrived for unknown channel " + current_channel_id + ", ignoring");
                return;
            }

            if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id == msg.message.sender_id)
            {
                console.log("received keys for channel '" + g_channel_list[index].name + "' from user : '" + msg.message.sender_username + "'. Setting new value of current_channel_keys");
                // if (current_channel_keys != null)
                // {
                //    g_historic_keys_of_current_channel.push(current_channel_keys);
                // }
                current_channel_keys = JSON.parse(msg.message.some_json.value);
                console.log("current_channel_keys -> ", current_channel_keys);

                // send channel keys to data processing worker and to opus g_decoder worker
                // webworkers have access to the global variable named "current_channel_keys" too.. but they have to be reinitialized again
                // globals exist multiple times in paralel, think of it as paralel reality where same globals exist within program at the same time, only one program exist in reality A (UI thread) and second global with same name in reality B (webworker). Its how globals work in webworkers... Webworkers arent men to be used in same file in first place
                // this was a hack so.
                // anyways this must be done, so webworkers that deal with data are also aware of what channel keys are

                g_data_processing_worker.postMessage({
                    type: "mainthread__channel_keys_for_data_processing_worker",
                    value: current_channel_keys
                });

                g_opus_decoder_worker.postMessage({
                    type: "mainthread__channel_keys_for_opus_decoder",
                    value: current_channel_keys
                });

                // valid keys arrived from the announced maintainer - stop the reset countdown
                cancel_maintainer_keys_wait_timer();

            }
            else
            {
                console.log("received keys for channel '" + g_channel_list[index].name + "' from user : '" + msg.message.sender_username + "' The user is not maintainer of current channel. Refusing to set keys");
                console.log("channel_list[index].maintainer_id = " + g_channel_list[index].maintainer_id);
                console.log("msg.message.sender_id = " + msg.message.sender_id);
            }
        }
    },
    // fills a private chat picture placeholder with the decrypted image and scrolls down
    process_direct_chat_picture_from_server: function(msg)
    {
        if (document.getElementById("chat-picture-img-" + msg.message.picture_id) != null)
        {
            document.getElementById("chat-picture-img-" + msg.message.picture_id).src = msg.message.decrypted_base64_picture;
            document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;
            document.getElementById("chat-picture-img-" + msg.message.picture_id).classList.remove("chat-picture-img-default");
        }
    },
    // fills a channel chat picture placeholder with the decrypted image and scrolls down
    process_channel_chat_picture_from_server: function(msg)
    {
        if (document.getElementById("chat-picture-img-" + msg.message.picture_id) != null)
        {
            document.getElementById("chat-picture-img-" + msg.message.picture_id).src = msg.message.decrypted_base64_picture;
            document.getElementById("chat-picture-img-" + msg.message.picture_id).classList.remove("chat-picture-img-default");
            document.getElementById("chat-context-container").scrollTop = document.getElementById("chat-context-container").scrollHeight;
        }
    },
    // upload confirmed: strips the "imgnotsentyet" marker class from every element carrying it
    process_image_sent_status_from_server: function(msg)
    {
        var lights = document.getElementsByClassName("imgnotsentyet");
        while (lights.length)
        {
            lights[0].classList.remove("imgnotsentyet");
        }
    },
    // moves a client into the idle list (channel_id -2, is_idle true), re-renders his row
    // there with handlers, plays the leave sound and stops our own audio sending
    process_client_client_going_to_idle_mode_from_server: function(msg)
    {
        let client_old_channel_id = get_client_by_client_id(msg.message.client_id).channel_id;
        let local_client_channel_id = get_client_by_client_id(local_client_id).channel_id;

        g_client_list[get_client_index_in_array_by_client_id(msg.message.client_id)].channel_id = -2;
        g_client_list[get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = true;

        let single_client = g_client_list[get_client_index_in_array_by_client_id(msg.message.client_id)];

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        if (local_client_id != single_client.client_id)
        {
            html_to_append = generate_html_for_single_client(single_client, false);
        }
        else
        {
            html_to_append = generate_html_for_single_client(single_client, true);
        }

        get_channel_own_clients_last_element("idle").insertAdjacentHTML("afterend", html_to_append);

        let element = document.querySelector('.connected-client[data-connected-client-id="'+msg.message.client_id+'"]');

        if (g_is_client_running_under_touch_device)
        {
            let local_touch_press_timer = null; // for touch devices

            element.addEventListener("touchstart", (event) => {

                g_is_long_press = false;

                // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                // by the time setTimeout runs, event object (or at least part of it) is lost,
                // things from event object must be imediatelly stored in temp variable in this case, for later use

                const currentTarget = event.currentTarget;
                const clientY = event.touches[0].clientY;
                const clientX = event.touches[0].clientX;

                local_touch_press_timer = window.setTimeout( () => {

                    g_is_long_press = true;
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                },
                600, event);

            });

            element.addEventListener("touchend", (event) => {

                clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                const currentTarget = event.currentTarget;
                const clientY = event.changedTouches[0].pageY;
                const clientX = event.changedTouches[0].pageX;

                if (g_is_long_press == false)
                {
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                }
            });
        }
        else
        {
            element.addEventListener("mousedown", UI.connected_user_onmousedown);
        }

        if (local_client_id != msg.message.client_id && local_client_channel_id == client_old_channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_left_your_channel.play();
            }
        }

        // stop sending audio if it was being sent
        process_stop_sending_audio();

    },
    // updates the renamed client's username in g_client_list, on his row (or the local
    // input for ourselves) and on his chat context pill if one exists
    process_client_rename_from_server: function(msg)
    {
        let client_index = get_client_index_in_array_by_client_id(parseInt(msg.message.client_id));
        if (client_index == -1)
        {
            return;
        }
        custom_log(g_client_list[client_index].username + ' renamed to -> ' + msg.message.new_username);
        g_client_list[client_index].username = msg.message.new_username;

        if (msg.message.client_id != local_client_id)
        {
            // null-guarded: a client in a hidden channel has no painted row
            let renamed_row = document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]');

            if (renamed_row != null && renamed_row.getElementsByClassName("connected-client-p")[0] != null)
            {
                renamed_row.getElementsByClassName("connected-client-p")[0].textContent = msg.message.new_username;
            }
        }
        else
        {
            // our own chat messages render from g_local_username, so keep it in sync
            g_local_username = sanitize_string(msg.message.new_username);
            { let rename_input = document.getElementById("connected-local-client-input"); if (rename_input != null) { rename_input.value = msg.message.new_username; } }
        }

        let is_found = document.querySelector('[data-chat-context-selector-id="user-' + msg.message.client_id + '"]');
        if (is_found != null)
        {
            // the pill is labelled with the alias when there is one, so a rename must not
            // paint the username over it
            is_found.getElementsByClassName("p-container")[0].textContent = get_display_name_by_client_id(msg.message.client_id, msg.message.new_username);
        }

        // messages already on screen keep the old name without this
        retag_rendered_messages_of_sender(msg.message.client_id, get_display_name_by_client_id(msg.message.client_id, msg.message.new_username));
    },
    // applies the received base64 avatar to every UI spot showing that client
    process_client_avatar_from_server: function(msg)
    {
        apply_avatar_to_ui(msg.message.client_id, msg.message.base64_avatar);
    },
    // the server tells us a client's avatar changed (no image payload): forget our cached copy
    // and re-fetch it if we might be showing that client.
    process_avatar_changed_from_server: function(msg)
    {
        if (g_avatars_allowed == false) { return; }
        let client_object = get_client_by_client_id(msg.message.client_id);
        if (client_object != null) { client_object.base64_avatar = null; }
        request_single_avatar(msg.message.client_id);
    },
    // one-shot initial roster: fills g_client_list (+ id map), identifies the local client,
    // renders every visible row with handlers, then kicks off the avatar lazy-loading
    process_client_list_from_server: function(msg)
    {
        if (typeof window === 'undefined')
        {
            return;
        }

        if (g_is_client_list_retrieved == true)
        {
            custom_log("client_list received more than once. Server is doing something weird");
            return;
        }

        console.log(msg);

        g_is_client_list_retrieved = true;

        for (let i = 0; i < msg.message.clients.length; i++)
        {
            let username = sanitize_string(msg.message.clients[i].username);
            let client_id = msg.message.clients[i].client_id;

            // clients channel is hidden (-1)
            if (msg.message.clients[i].channel_id == -1)
            {
                let single_client = {
                    client_id: null,
                    username: null,
                    alias: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    tag_ids: null,
                    is_clients_channel_hidden: null,
                    country_iso_code: null,
                    is_idle: null,
                    is_ignored_by_local_client: null,
                    is_muted_by_local_client: null,
                    unread_count: 0,
                    is_music_bot: null
                };

                single_client.client_id = parseInt(client_id);
                single_client.username = username;
                single_client.alias = (msg.message.clients[i].alias != null) ? msg.message.clients[i].alias : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.tag_ids = msg.message.clients[i].tag_ids;
                single_client.is_clients_channel_hidden = msg.message.clients[i].is_clients_channel_hidden;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
            }
            else if (msg.message.clients[i].is_idle == true)
            {

                let single_client = {
                    client_id: null,
                    username: null,
                    alias: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    tag_ids: null,
                    is_clients_channel_hidden: null,
                    country_iso_code: null,
                    is_idle: null,
                    is_ignored_by_local_client: null,
                    is_muted_by_local_client: null,
                    unread_count: 0,
                    is_music_bot: null
                };

                single_client.client_id = parseInt(client_id);
                single_client.username = username;
                single_client.alias = (msg.message.clients[i].alias != null) ? msg.message.clients[i].alias : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.tag_ids = msg.message.clients[i].tag_ids;
                single_client.is_clients_channel_hidden = msg.message.clients[i].is_clients_channel_hidden;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);

                let indentation_level = 1;

                if (username != sanitize_string(msg.message.local_username))
                {
                    html_to_append = generate_html_for_single_client(msg.message.clients[i], false);
                }
                else
                {
                    // this else statement will never run
                    g_local_username = sanitize_string(msg.message.local_username);
                    local_client_id = msg.message.clients[i].client_id;
                    html_to_append = generate_html_for_single_client(msg.message.clients[i], true);
                }

                document.querySelector('[data-channel-id="idle"]').insertAdjacentHTML("afterend", html_to_append);
            }
            else
            {
                let html_to_append = "";

                if (username != sanitize_string(msg.message.local_username))
                {
                    html_to_append = generate_html_for_single_client(msg.message.clients[i], false);
                }
                else
                {
                    g_local_username = sanitize_string(msg.message.local_username);
                    local_client_id = msg.message.clients[i].client_id;
                    html_to_append = generate_html_for_single_client(msg.message.clients[i], true);
                }

                document.querySelector('[data-channel-id="' + msg.message.clients[i].channel_id + '"]').insertAdjacentHTML("afterend", html_to_append);

                let single_client = {
                    client_id: null,
                    username: null,
                    alias: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    tag_ids: null,
                    is_clients_channel_hidden: null,
                    country_iso_code: null,
                    is_idle: null,
                    is_ignored_by_local_client: null,
                    is_muted_by_local_client: null,
                    unread_count: 0,
                    is_music_bot: null
                };

                // they killed the subcultures they cancelled the future and for what, for you to not be offended

                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.tag_ids = msg.message.clients[i].tag_ids;
                single_client.is_clients_channel_hidden = false;
                single_client.client_id = parseInt(client_id);
                single_client.username = username;
                single_client.alias = (msg.message.clients[i].alias != null) ? msg.message.clients[i].alias : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
            }
        }

        for (let x = 0; x < g_chat_context_array.length; x++)
        {
            if (g_chat_context_array[x].type == "user")
            {
                for (let y = 0; y < msg.message.clients.length; y++)
                {
                    // .client_id, not .id - the payload has no `id` field (server_message.c:382),
                    // so this comparison never matched and the else below fired for EVERY open
                    // pm context. that was harmless only because the getElementById below was
                    // misspelled and threw, killing the loop. the two have to be fixed together:
                    // correcting the spelling alone would start deleting every pm context on
                    // every client_list
                    if (g_chat_context_array[x].chat_context_id == "chat-context-pm-" + msg.message.clients[y].client_id)
                    {
                        break;
                    }
                    else
                    {
                        if (y + 1 == msg.message.clients.length)
                        {
                            custom_log("chat context with id " + g_chat_context_array[x].chat_context_id.toString() + " NOT FOUND");
                            document.getElementById(g_chat_context_array[x].chat_context_id).remove();
                        }
                    }
                }
            }
        }

        let elements = document.getElementsByClassName('connected-client');
        for (let i = 0; i < elements.length; i++)
        {
            // dont want to use this handler when local client is clicked
            // let status = elements[i].classList.contains("connected-local-client");
            // if (status)
            // {
            //    continue;
            // }

            if (g_is_client_running_under_touch_device)
            {
                let local_touch_press_timer = null; // for touch devices

                elements[i].addEventListener("touchstart", (event) => {

                    g_is_long_press = false;

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    local_touch_press_timer = window.setTimeout( () => {

                        g_is_long_press = true;
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {

                    clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                    const currentTarget = event.currentTarget;
                    const clientY = event.changedTouches[0].pageY;
                    const clientX = event.changedTouches[0].pageX;

                    if (g_is_long_press == false)
                    {
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                    }
                });

            }
            else
            {
                elements[i].addEventListener("mousedown", UI.connected_user_onmousedown);
            }

            elements[i].style.backgroundColor = "";
        }
        { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout); } }

        if (g_is_running_in_android_webview == true)
        {
            Android.JavaExportOnConnected();
        }

        UI.refresh_all_channel_fullness();

        // the full client list is in: lazy-load everyone's avatars in growing chunks
        enqueue_all_avatars_for_loading();

        // themes that do not show the avatar grid get the paced one-at-a-time prefetch instead
        start_avatar_prefetch();

        // a strip theme saved from last time is applied at startup, BEFORE the
        // server config could set g_avatars_allowed - so the avatar grid never armed and
        // the enqueue above bailed. re-evaluate now that both the flag and the clients
        // exist; if the grid just armed, refresh does the initial bulk enqueue itself.
        UI.refresh_member_list_state();
    },
    // shows the "insufficient permissions" alert and plays its sound effect
    process_access_denied_from_server: function(msg)
    {
        custom_alert("insufficient permissions");
        g_sound_effects.insufficient_permissions.play();
    },
    // handles anyone switching channel: moves his row, replays his tag chips, plays join/leave/
    // switch sounds; for the local client also swaps chat context and nulls current_channel_keys
    process_channel_join_from_server: function(msg)
    {
        let client_that_joined = g_client_list[get_client_index_in_array_by_client_id(msg.message.client_id)];
        let client_old_channel_id = client_that_joined.channel_id;
        client_that_joined.channel_id = msg.message.channel_id;

        // check if client went into unknown channel (-1)
        // or known channel

        if (msg.message.channel_id == -1)
        {
            // this is case where client left to other unknown channel
            client_that_joined.is_clients_channel_hidden = true;
            document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }
        else
        {
            let local_client_channel_id = get_client_by_client_id(local_client_id).channel_id;

            if (local_client_channel_id == msg.message.channel_id && local_client_id != msg.message.client_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.user_joined_your_channel.play();
                }
            }
            else if (local_client_id == msg.message.client_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.channel_switched.play();
                }
            }
            else if (local_client_id != msg.message.client_id && local_client_channel_id == client_old_channel_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.user_left_your_channel.play();
                }
            }

            if (msg.message.client_id == local_client_id)
            {
                audio_player_clear();

                let marquee_containers = document.getElementsByClassName("marquee-music-playing-container");
                for (let i = 0; i < marquee_containers.length; i++)
                {
                    marquee_containers[i].style.display = "none";
                }

                document.getElementById("channel-password-enter-container").style.display = "none";
                document.getElementById("background-container").style.display = "none";
                document.getElementsByClassName("connected-local-client")[0].remove();

                html_to_append = generate_html_for_single_client(client_that_joined, true);

                get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);
                { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout); } }

                current_channel_id = msg.message.channel_id;
                current_channel_keys = null;

                console.log("local_user joined new channel, nulling out current_channel_keys");

                let is_found = document.querySelector('[data-chat-context-selector-id="channel-' + msg.message.channel_id + '"]');

                if (is_found == null)
                {
                    let channel_name = document.querySelector('[data-channel-name-id="' + msg.message.channel_id + '"]').innerHTML;
                    let to_append = "<div class=\"chat-context-selector\" data-chat-context-selector-type=\"channel\" data-chat-context-selector-id=\"channel-" + msg.message.channel_id + "\">\n\
                                        <div class=\"p-container\">\n\
                                            <p>" + channel_name + "</p>\n\
                                        </div>\n\
                                        <div class=\"remove-chat-context-selector\" data-chat-context-remove-selector-type=\"channel\" data-chat-context-remove-selector-id=\"channel-" + msg.message.channel_id + "\">\n\
                                        </div>\n\
                                    </div>";

                    document.getElementById("chat-context-selectors-container").insertAdjacentHTML("beforeend", to_append);

                    for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
                    {
                        document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
                    }

                    for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
                    {
                        document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
                    }
                }

                for (let i = 0; i < document.getElementsByClassName("chat-context-selector").length; i++)
                {
                    document.getElementsByClassName("chat-context-selector")[i].style.backgroundColor = "";
                }

                let elements1 = document.getElementsByClassName('connected-client');
                for (let i = 0; i < elements1.length; i++)
                {
                    elements1[i].style.backgroundColor = "";
                }

                document.querySelector('[data-chat-context-selector-id="channel-' + msg.message.channel_id + '"]').style.backgroundColor = "#36393f";

                // g_chat_context_array is the source of truth, not the dom. asking the dom whether the
                // context exists ties state creation to rendering, which breaks with no dom present
                let joined_channel_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
                let is_chat_context_existing = false;

                for (let context_index = 0; context_index < g_chat_context_array.length; context_index++)
                {
                    if (g_chat_context_array[context_index].chat_context_id == joined_channel_chat_context_id)
                    {
                        is_chat_context_existing = true;
                        break;
                    }
                }

                if (is_chat_context_existing == false)
                {
                    console.log("adding channel");

                    // the channel name is state. taking it out of the row's markup made the
                    // context label depend on the row having been painted first. sanitize_string
                    // here yields exactly what that markup already held, so the text is unchanged
                    let joined_channel = get_channel_by_id(g_channel_list, msg.message.channel_id);
                    let channel_name = (joined_channel != null) ? sanitize_string(joined_channel.name) : "";

                    let html_to_append3 = '<div class="chat-context" id="chat-context-channel-' + msg.message.channel_id + '" style="display: none;">\n\
                                                <div class="single-server-message">now talking channel: ' + channel_name + '</div>\n\
                                                    </div>\n\
                                                </div>\n\
                                            </div>';

                    // the sibling insert is kept rather than switched to a container append, so
                    // the node lands in the same place in the browser. only the blind [count - 1]
                    // is guarded, which is what made this throw with no contexts painted
                    let count = document.getElementsByClassName("chat-context").length;

                    if (count > 0)
                    {
                        document.getElementsByClassName("chat-context")[count - 1].insertAdjacentHTML("afterend", html_to_append3);
                    }

                    let single_chat_context = {
                        type: "channel",
                        chat_context_id: joined_channel_chat_context_id,
                        last_known_message_sender_username: ""
                    };

                    g_chat_context_array.push(single_chat_context);
                }

                let count1 = document.getElementsByClassName("chat-context").length;

                for (let i = 0; i < count1; i++)
                {
                    document.getElementsByClassName("chat-context")[i].style.display = "none";
                }

                document.getElementById("chat-context-channel-" + msg.message.channel_id).style.display = "block";

                g_current_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        clear_channel_unread_count(msg.message.channel_id); // opened it, so it is read
                g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_alias = ""; // back on a channel: no offline target
                console.log("joined new channel, nulling out current_channel_keys");
                current_channel_keys = null;

                // joined new channel, now loop through clients and set microphone state to 2 for all clients that have it set to 1
                // only clients in current channel should have microphone state set to 1
                // information about active mic state of clients in new channel is later received through websocket and processed

                for (var i = 0; i < g_client_list.length; i++)
                {
                    if (g_client_list[i].audio_state == AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
                    {
                        g_client_list[i].audio_state = AUDIO_STATE.PUSH_TO_TALK_ENABLED; // not active but enabled

                        let client = {
                            client_id: g_client_list[i].client_id,
                            audio_state: g_client_list[i].audio_state
                        };

                        process_audio_state_of_single_client(client);
                    }
                }

                UI.enable_inputs();
            }
            else
            {
                // this is branch that gets run if client that joined new channel isnt local client
                // this code tries to remove client in tree visually and create him later

                // only remove clients entry if client was not in hidden channel before joining new channel
                if (client_that_joined.is_clients_channel_hidden == false)
                {
                    document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]').remove();
                }
                else
                {
                    // client was in hidden channel before
                    client_that_joined.is_clients_channel_hidden = false;
                }

                html_to_append = generate_html_for_single_client(client_that_joined, false);

                get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

                if (msg.message.channel_id == current_channel_id)
                {
                    console.log("user '" + client_that_joined.username + "' joined current_channel where local_user is. setting current_channel_keys to null");
                    current_channel_keys = null;
                    let channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, current_channel_id);
                    if (g_channel_list[channel_index].has_maintainer && g_channel_list[channel_index].maintainer_id == local_client_id)
                    {
                        console.log("local_user is the maintainer of current_channel. create_and_send_new_channel_keys()");
                        create_and_send_new_channel_keys();
                    }
                    else
                    {
                        console.log("local user is not maintainer of current channel. Waiting for new keys");

                        // somebody joined -> the maintainer must re-key the whole channel;
                        // if the fresh keys never arrive, vote for a maintainer reset
                        arm_maintainer_keys_wait_timer();
                    }
                }
            }

            // html element that represents client is now appended to newly joined channel at this point
            // now append client g_tags to client again

            for (let y = 0; y < client_that_joined.tag_ids.length; y++)
            {
                let tag = get_tag_by_tag_id(client_that_joined.tag_ids[y]);
                if (tag == null)
                {
                    console.log("tag is null");
                    continue;
                }
                let target_element = document.getElementById("client-tags-" + client_that_joined.client_id);
                const node = document.createElement("div");
                node.className = "single-tag";
                node.setAttribute("tag-id", tag.tag_id);

                // same guard as everywhere else: this is the path that runs when somebody switches
                // channel, which is why an unassigned icon used to appear only after a channel change
                let icon = (tag.has_icon == true) ? get_icon_by_icon_id(tag.tag_linked_icon_id) : null;
                if (icon != null)
                {
                    node.style.backgroundImage = "url("+icon.base64_icon+")";
                }
                target_element.appendChild(node);
            }

            let elements = document.getElementsByClassName('connected-client');

            for (let i = 0; i < elements.length; i++)
            {
                // dont want to use this handler when local client is clicked
                // let status = elements[i].classList.contains("connected-local-client");
                // if (status)
                // {
                //    continue;
                // }

                if (g_is_client_running_under_touch_device)
                {
                    let local_touch_press_timer = null; // for touch devices

                    elements[i].addEventListener("touchstart", (event) => {

                        g_is_long_press = false;

                        // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                        // by the time setTimeout runs, event object (or at least part of it) is lost,
                        // things from event object must be imediatelly stored in temp variable in this case, for later use

                        const currentTarget = event.currentTarget;
                        const clientY = event.touches[0].clientY;
                        const clientX = event.touches[0].clientX;

                        local_touch_press_timer = window.setTimeout( () => {

                            g_is_long_press = true;
                            UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                        },
                        600, event);

                    });

                    elements[i].addEventListener("touchend", (event) => {

                        clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                        const currentTarget = event.currentTarget;
                        const clientY = event.changedTouches[0].pageY;
                        const clientX = event.changedTouches[0].pageX;

                        if (g_is_long_press == false)
                        {
                            UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                        }
                    });

                }
                else
                {
                    elements[i].addEventListener("mousedown", UI.connected_user_onmousedown);
                }
            }

            if (msg.message.is_streaming_song)
            {
                let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + msg.message.client_id + '"]');
                if (element != null)
                {
                    element.style.display = "inline-block";
                    document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = sanitize_string(msg.message.song_name);
                }
                else
                {
                    console.log("could not find element");
                }
            }
        }

        UI.refresh_all_channel_fullness();
    },
    // blanks a deleted chat message to "deleted" (or resets a picture to the placeholder)
    // after checking the requester is the recorded author or an admin
    process_chat_message_delete_from_server: function(msg)
    {
        // the server tells us WHO asked for the delete; only honour it if that requester is the message's
        // author (recorded at render) or an admin. otherwise this client keeps the message - its prerogative.
        let recorded_author_public_key = g_chat_message_author_public_keys[msg.message.chat_message_id];
        if (recorded_author_public_key !== undefined && recorded_author_public_key !== msg.message.requester_public_key && msg.message.requester_is_admin !== true)
        {
            return;
        }

        // first try to find chat message under chat_message id, if not found try to find picture

        let element = document.querySelector('.single-chat-message-content-p[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');
        if (element != null)
        {
            element.removeEventListener("mousedown", UI.single_chat_message_onrightclick);
            element.innerHTML = "deleted";
            element.style.fontStyle = "italic";
            element.style.fontSize = "10px";

            if (element.classList.contains("local-single-chat-message-content-p"))
            {
                element.classList.remove("local-single-chat-message-content-p");
            }
        }
        else
        {
            let element = document.querySelector('.chat-picture-img[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');
            if (element != null)
            {
                element.removeEventListener("mousedown", UI.single_chat_message_onrightclick);
                element.src = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABgAAAAYCAYAAADgdz34AAADF0lEQVRIx71VsU5cRxQ9d98s1UoUISYFJUIgpUDySiloUIqUTmeUwo27lPmCOHWadEhUFBRQOK7sD9hNEcWhoDG4iGQs2ayNFBuZArN7z0lx580+bC/QxCPtvpl5896599xz7gP+52HXObS1tbU2Pz//A0mQhLtrfX39x+3t7cFVz6brAHQ6na+Xl5e/H7nDR47z4ZAkfwEwuFYGm5ubt6anpxcllRv1nCTm5ua+W1xc/NadcLr5yPXw0cPfOp3OwPJZCRAESKCEfq93f2Nj458EAEtLS3e73e4tAKYYUJ5DgiSTJMYcorB2e+0nibFHGSVJhChQsr8fP34NIABUPxiL8fqDXxMg1uWFUHNOQpmdBAC7u7t/npycfLWwsPBNRAsAJXIogrAxOPK+TBTUOPv27cnp88PDv07fnb4oADs7O7+urKz0V1dX/wDqRGTjUsiCYqGmsXENwEDH3t7e8wcPfr99fHz8BgBaANDr9TylNEwpIaWEKiVUVYXhcIiUKqSUkKqE0WgEM4szVYK1WhgOR6hShVRVqKqEqqp0fn7+vt/vswA0R6bCXr16jSdP9s3dCx37+wf28uio0DE4GtjBwYGJrPesIcSPfZD5VkiNBkAkYWYmIERFZqXBSCoX31BUqssBMrehiHhZDWzh5LqgAEXziD48VXvhUwAKjyAHConmdBXVQPJoFcZANFGiO0QZLGc4OQMVJ0oAKdDZ8IhA99B7jqbuTbFGLdcrKJIsdE+Q3qBIRnr9Qqvr5O4Xqb0MIBtJAkDK3KMVtESTIHeClCk4MpIivQhCQs3zJSqqG1dkcGGvkUFuGYR7tAjYuOlNKHL0EdQKIUF3EwW2BAhGJyhGxHEmaIsalFZyVQ2Ui2zurJVhkuT0D3wgRQ1o4+Z7GQBrJQFTU220qhbMrOynlNBuT5V1u91GSiloEUNpkwBUnBxtYGbmS3wxM2NN6m52uyjOlTA7O2s3Zm+gkeXkDBp9P2CUrVNAiwMtT8cfJ8nGBp0AcPjs2dN7936+I6CdG/ZH31dd/PvkvbOzs39JvsfnGv8B0U6eGZr+co0AAAAASUVORK5CYII=";
                element.classList.add("chat-picture-img-default");
            }
            else
            {
                console.log("process_chat_message_delete_from_server unable to find element");
            }
        }
    },
    // shows the poke text in an alert with the poke sound, unless the poker is ignored
    process_poke_from_server: function(msg)
    {
        // the sender is not always in our client list - somebody in a hidden password
        // channel is left out of it, and a poke can also land before the list finished
        // building. dereferencing that missing entry threw and killed this whole handler,
        // so the poke was lost with no sound, no alert and no android notification
        let poke_sender = get_client_by_client_id(msg.message.client_id);

        if (poke_sender != null && poke_sender.is_ignored_by_local_client == true)
        {
            return;
        }

        if (g_are_sound_effects_enabled)
        {
            g_sound_effects.poke.play();
        }
        let poke_sender_name = get_display_name_by_client_id(msg.message.client_id, "");
        let string1 = "" + sanitize_string(poke_sender_name) + " says : " + sanitize_string(msg.message.poke_message);
        custom_alert(string1);

        // in the wrapper app the alert above is drawn inside a webview nobody is looking
        // at while the app is in the background - which is when a poke actually matters.
        // hand it to android too, it decides whether a notification is needed
        if (g_is_running_in_android_webview == true && typeof Android !== "undefined")
        {
            try { Android.JavaExportShowPokeNotification(poke_sender_name, msg.message.poke_message); }
            catch (e) { console.warn("poke notification bridge failed: " + e.message); }
        }
    },
    // replaces an edited message's text in place (shown pink) if the requester
    // is the recorded author or an admin
    process_chat_message_edit_from_server: function(msg)
    {
        let recorded_author_public_key = g_chat_message_author_public_keys[msg.message.chat_message_id];
        if (recorded_author_public_key !== undefined && recorded_author_public_key !== msg.message.requester_public_key && msg.message.requester_is_admin !== true)
        {
            return;
        }

        let element = document.querySelector('.single-chat-message-content-p[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');
        if (element != null)
        {
            element.innerHTML = sanitize_string(msg.message.new_message_value);
            element.style.color = "pink";
        }
    },
    // pushes the new icon into g_icons, appends its settings entry, and (if this
    // reply is our in-flight upload) sends the next queued icon upload
    process_icon_add_from_server: function(msg)
    {
        let icon = {
            id: msg.message.icon_id,
            base64_icon: msg.message.base64_icon
        }

        g_icons.push(icon);

        let html_to_append = "<div class='server-settings-icon-entry' data-icon-id="+icon.id+"><img class='img-uploaded-icon' src="+icon.base64_icon+"></img><button class='settings-entry-delete-button' title='delete icon'>✕</button></div>";

        document.getElementById("server-settings-tab-icons-container").insertAdjacentHTML("beforeend", html_to_append);

        // batch upload: if this reply is the icon we just sent, send the next queued one
        if (g_icon_upload_in_flight_base64 != null && msg.message.base64_icon == g_icon_upload_in_flight_base64)
        {
            g_icon_upload_in_flight_base64 = null;
            send_next_queued_icon_upload();
        }
    },
    // drops the tag id from the client's tag_ids and removes its chip from his row
    process_remove_tag_from_client_from_server: function(msg)
    {
        console.log(msg);
        let client = get_client_by_client_id(msg.message.client_id);

        if (client == null)
        {
            return;
        }

        // do not add duplicate tag ids to client object

        if (client.tag_ids.includes(msg.message.tag_id))
        {
            client.tag_ids.splice(client.tag_ids.indexOf(msg.message.tag_id), 1);  // deleting
        }

        // double selector..first find client g_tags element
        let client_tags = document.getElementById("client-tags-"+msg.message.client_id);
        let target_element = client_tags.querySelector('.single-tag[tag-id="'+msg.message.tag_id+'"]');
        if (target_element != null)
        {
            target_element.remove();
        }
        else
        {
            console.log("process_remove_tag_from_client_from_server failed to find tag-id:" + msg.message.tag_id);
        }
    },
    // one-shot snapshot of the identities the server stores: [{alias, base64_avatar, tag_ids}].
    // it carries no ids or keys - the alias is the handle, and the server keeps aliases unique,
    // so an entry whose alias matches a connected client IS that client and is not listed again
    process_stored_clients_list_from_server: function(msg)
    {
        g_offline_client_list = [];

        if (msg.message.stored_clients == null)
        {
            return;
        }

        for (let i = 0; i < msg.message.stored_clients.length; i++)
        {
            let entry = msg.message.stored_clients[i];

            if (entry == null || typeof entry.alias !== "string" || entry.alias.length == 0)
            {
                continue; // nothing to name or pair it by
            }

            g_offline_client_list.push({
                alias: entry.alias,
                base64_avatar: (typeof entry.base64_avatar === "string") ? entry.base64_avatar : "",
                tag_ids: (entry.tag_ids != null) ? entry.tag_ids : [],
                // unix seconds; only sent when the server has last-seen enabled
                last_seen: (typeof entry.last_seen === "number") ? entry.last_seen : 0,
                // only sent when the server has offline messages enabled. having it IS the
                // permission to write to this person while they are away - without their
                // public key there is no way to encrypt anything for them
                public_key: (typeof entry.public_key === "string") ? entry.public_key : ""
            });
        }

        UI.schedule_member_list_sync();
    },

    // updates the client's alias in g_client_list and on his rows (text + has-alias
    // class), then schedules a member-list sync to mirror it
    process_client_alias_changed_from_server: function(msg)
    {
        let client_object = get_client_by_client_id(msg.message.client_id);
        if (client_object == null)
        {
            return;
        }

        client_object.alias = (msg.message.alias != null) ? msg.message.alias : "";

        // an alias change changes the display name on old messages too
        retag_rendered_messages_of_sender(msg.message.client_id, get_display_name_by_client_id(msg.message.client_id, client_object.username));

        // update the live rows in place; the member-list strip mirror re-clones them right after
        let rows = document.querySelectorAll('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]');
        for (let i = 0; i < rows.length; i++)
        {
            let alias_p = rows[i].querySelector(".client-alias");
            if (alias_p == null)
            {
                continue;
            }

            alias_p.textContent = client_object.alias;

            if (client_object.alias.length > 0)
            {
                rows[i].classList.add("has-alias");
            }
            else
            {
                rows[i].classList.remove("has-alias");
            }
        }

        // an open chat with this person is labelled by name too - relabel it now instead
        // of leaving the old username sitting there until the chat is reopened
        let open_pill = document.querySelector('[data-chat-context-selector-id="user-' + msg.message.client_id + '"]');
        if (open_pill != null)
        {
            open_pill.getElementsByClassName("p-container")[0].textContent = get_display_name_by_client_id(msg.message.client_id, client_object.username);
        }

        UI.schedule_member_list_sync();
    },

    // records the tag on the client (no duplicates) and paints its chip; also reveals
    // the server-settings button when the local client just received the admin tag
    process_add_tag_to_client_from_server: function(msg)
    {
        let client = get_client_by_client_id(msg.message.client_id);

        if (client == null)
        {
            return;
        }

        // do not add duplicate tag ids to client object

        let tag = get_tag_by_tag_id(msg.message.tag_id);

        // resolve the tag BEFORE recording it - an unknown id used to be pushed onto the
        // client and only then bailed on, leaving an id no later render could resolve
        if (tag == null)
        {
            console.log("tag is null");
            return;
        }

        if (!client.tag_ids.includes(msg.message.tag_id))
        {
            client.tag_ids.push(msg.message.tag_id);
        }

        let target_element = document.getElementById("client-tags-" + msg.message.client_id);

        // a client in a hidden channel has no row to paint on. this used to throw here and
        // skip everything below it, including revealing the local admin's settings button
        if (target_element != null)
        {
            const node = document.createElement("div");
            node.className = "single-tag";
            node.setAttribute("tag-id", tag.tag_id);

            let icon = tag.has_icon ? get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

            if (icon != null)
            {
                node.style.backgroundImage = "url("+icon.base64_icon+")";
            }

            target_element.appendChild(node);
        }

        if (msg.message.client_id == local_client_id && msg.message.tag_id == 0) // admin tag id
        {
            document.getElementById("enter-server-settings").style.display = "block";
            document.getElementById("enter-server-settings").onclick = UI.enter_server_settings_onclick;
        }
    },
    // pushes the newly created tag into g_tags and appends its row to the settings tag table
    process_tag_add_from_server: function(msg)
    {
        let tag = msg.message;

        g_tags.push(tag);

        let icon = tag.has_icon ? get_icon_by_icon_id(tag.tag_linked_icon_id) : null;

        let base64_icon = "";

        if (icon != null)
        {
            base64_icon = icon.base64_icon;
        }

        let tag_delete_button_html = (tag.tag_id != 0) ? "<button class=\"settings-entry-delete-button\" title=\"delete tag\">✕</button>" : "";
        let html_to_append = "<div class=\"server-settings-tag-entry\" data-tag-id=\""+tag.tag_id+"\">\n\
                                <p class=\"tag-settings-entry-p\">"+tag.tag_id+"</p>\n\
                                <p class=\"tag-settings-entry-p\">"+tag.tag_name+"</p>\n\
                                <p class=\"tag-settings-entry-p\">"+tag.tag_linked_icon_id+"</p>\n\
                                <div class=\"tag-settings-entry-img\" style=\"background-image: url("+base64_icon+");\"></div>\n\
                                "+tag_delete_button_html+"\n\
                            </div>";

        document.getElementById("server-settings-tab-tags-container").insertAdjacentHTML("beforeend", html_to_append);
    },
    // removes the tag from g_tags, its settings row, every displayed chip
    // and every client's tag_ids
    process_tag_delete_from_server: function(msg)
    {
        let tag_id = msg.message.tag_id;

        for (let i = g_tags.length - 1; i >= 0; i--)
        {
            if (g_tags[i].tag_id == tag_id) { g_tags.splice(i, 1); }
        }

        let entry = document.querySelector('.server-settings-tag-entry[data-tag-id="' + tag_id + '"]');
        if (entry != null) { entry.remove(); }

        let displayed_tags = document.querySelectorAll('.single-tag[tag-id="' + tag_id + '"]');
        for (let i = 0; i < displayed_tags.length; i++) { displayed_tags[i].remove(); }

        for (let i = 0; i < g_client_list.length; i++)
        {
            let index_of_tag = g_client_list[i].tag_ids.indexOf(tag_id);
            if (index_of_tag != -1) { g_client_list[i].tag_ids.splice(index_of_tag, 1); }
        }
    },
    // removes the icon from g_icons and its entry from the settings icon list
    process_icon_delete_from_server: function(msg)
    {
        let icon_id = msg.message.icon_id;

        for (let i = g_icons.length - 1; i >= 0; i--)
        {
            if (g_icons[i].id == icon_id) { g_icons.splice(i, 1); }
        }

        let entry = document.querySelector('.server-settings-icon-entry[data-icon-id="' + icon_id + '"]');
        if (entry != null) { entry.remove(); }
    },
    // stores the channel's new icon fields and repaints its row (and the icon box
    // of the channel edit form if it is open for that channel)
    process_channel_icon_changed_from_server: function(msg)
    {
        let channel = get_channel_by_id(g_channel_list, msg.message.channel_id);
        if (channel != null)
        {
            channel.has_channel_icon = msg.message.has_channel_icon;
            channel.channel_icon_id = msg.message.channel_icon_id;
            UI.refresh_channel_icon(channel);

            // if the edit form is open for this channel, update its icon box too
            if (g_channel_properties_edit_channel_id == msg.message.channel_id)
            {
                UI.refresh_channel_edit_icon_box(channel);
            }
        }
    },
    // updates the tag's icon fields in g_tags and repaints its settings row
    // plus every displayed chip of that tag
    process_tag_icon_changed_from_server: function(msg)
    {
        let tag_id = msg.message.tag_id;
        let has_icon = msg.message.has_icon;
        let icon_id = msg.message.tag_linked_icon_id;

        for (let i = 0; i < g_tags.length; i++)
        {
            if (g_tags[i].tag_id == tag_id)
            {
                g_tags[i].has_icon = has_icon;
                g_tags[i].tag_linked_icon_id = icon_id;
                break;
            }
        }

        let icon = has_icon ? get_icon_by_icon_id(icon_id) : null;
        let background = (icon != null) ? ("url(" + icon.base64_icon + ")") : "";

        let entry = document.querySelector('.server-settings-tag-entry[data-tag-id="' + tag_id + '"]');
        if (entry != null)
        {
            let img_box = entry.querySelector('.tag-settings-entry-img');
            if (img_box != null) { img_box.style.backgroundImage = background; }
        }

        let displayed_tags = document.querySelectorAll('.single-tag[tag-id="' + tag_id + '"]');
        for (let i = 0; i < displayed_tags.length; i++)
        {
            displayed_tags[i].style.backgroundImage = background;
        }
    },
    // shows the marquee on the streaming client's row with the streamed song name
    process_start_song_stream_from_server: function(msg)
    {
        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + msg.message.client_id + '"]');
        if (element != null)
        {
            element.style.display = "inline-block";
            document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = sanitize_string(msg.message.song_name);
        }
        else
        {
            console.log("could not find element");
        }
    },

    // opens the musicbot management dialog and rebuilds its song table;
    // each row's X button sends a remove-song request for that song
    process_music_bot_song_list_from_server: function(msg)
    {
        console.log(msg);

        document.getElementById("musicbot-management-background-container").style.display = "block";

        let html_to_append = "";

        for (single_song of msg.message.songs)
        {
            html_to_append += '\n\
            <tr style="border-bottom: 1px solid #eee;">\n\
                <td style="padding:10px;">'+ single_song.name +'</td>\n\
                <td style="padding:10px; text-align:right;">'+ single_song.duration_seconds +'</td>\n\
                <td style="padding:10px; text-align:right; cursor:pointer;"><input class="remove-song-from-musicbot-button" type="button" data-song-id="'+single_song.id+'" value="X"></td>\n\
            </tr>';
        }

        document.getElementById("musicbot-management-background-container-tbody").innerHTML = html_to_append;

        for (let x = 0; x < document.getElementsByClassName("remove-song-from-musicbot-button").length; x++)
        {
            document.getElementsByClassName("remove-song-from-musicbot-button")[x].onclick = function(event) {
                event.stopPropagation();

                let song_id = parseInt(event.currentTarget.getAttribute("data-song-id"));

                client_msg.send_remove_song_from_music_bot_request(song_id, parseInt(selected_client_id));
            };
        }
    },

    // upload acknowledged: tells the server what the finished file is for
    // by sending the current file_send_intent back
    process_file_send_success_from_server: function(msg)
    {
        // the ack means every part actually arrived, so this is where the upload is really
        // finished. released before the intent check so a stray ack cannot leave the lock stuck
        release_file_upload_lock();

        if (file_send_intent == "musicbot_file" || file_send_intent == "direct_chat_picture_file" || file_send_intent == "channel_chat_picture_file")
        {
            client_msg.send_file_send_completed_request(file_send_intent);

            // one upload = one completion. clear the intent so a later stray file_send_success
            // cannot replay the last file (e.g. re-upload the song on an unrelated action)
            file_send_intent = "";
            file_send_intent_extra_data = {};
        }
    },

    // accumulates one base64 chunk of an incoming file in g_received_files
    // (keyed by server_chat_message_id), refreshing its last-received timestamp
    process_file_receive_chunk_from_server_from_server: function(msg)
    {
        let is_found = false;
        for (i = 0; i < g_received_files.length; i++)
        {
            if(g_received_files[i].file_id == msg.message.server_chat_message_id)
            {
                is_found = true;
                break;
            }
        }

        if (is_found == false)
        {
            let single_file = {
                file_id: msg.message.server_chat_message_id,
                file_content_base64: "",
                timestamp_last_received: new Date().valueOf()
            };

            g_received_files.push(single_file);
        }

        for (i = 0; i < g_received_files.length; i++)
        {
            if(g_received_files[i].file_id == msg.message.server_chat_message_id)
            {
                g_received_files[i].timestamp_last_received = new Date().valueOf();
                g_received_files[i].file_content_base64 += msg.message.value;
                break;
            }
        }
    },

    // hands the fully received base64 file to g_data_processing_worker for decryption
    // (direct or channel chat picture) and removes it from g_received_files
    process_file_receive_completed_from_server_from_server: function(msg)
    {
        let message_raw = "";
        let index_to_delete = 0;
        let is_found = false;
        for (i = 0; i < g_received_files.length; i++)
        {
            if(g_received_files[i].file_id == msg.message.server_chat_message_id)
            {
                is_found = true;
                index_to_delete = i;
                message_raw = g_received_files[i].file_content_base64;
                break;
            }
        }

        if (is_found == false)
        {
            return;
        }

        // evict BEFORE the handoff, not after. the content is already captured in
        // message_raw, and this is the only removal from g_received_files anywhere in the
        // tree - so anything that stops the worker call from returning strands the entry,
        // and the chunk handler appends with += , meaning the next picture reusing this
        // message id would be built on top of the leftovers
        g_received_files.splice(index_to_delete, 1);

        if (msg.message.receive_type == "direct_chat_picture")
        {
            // loopback: no private key here, node sends the decrypted picture instead
            if (is_ui_only_runtime())
            {
                return;
            }

            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_direct_chat_picture_data",
                message_raw: message_raw,
                picture_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });

            console.log("total files in received_files " + g_received_files.length +"");
        }
        else if (msg.message.receive_type == "channel_chat_picture")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_channel_chat_picture_data",
                message_raw: message_raw,
                picture_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });

            console.log("total files in received_files " + g_received_files.length +"");
        }
    }
};

var client_msg = {
    // asks the server to move the local client into idle mode
    send_go_to_idle_mode_request: function()
    {
        let message_object = {
            message:
            {
                type: "go_to_idle_mode_request",
            }
        };

        send_message_object(message_object);
    },

    // asks for the identities the server has stored (alias/avatar/g_tags), so people who are
    // registered here can be shown while offline. server ignores it unless it allows the list
    send_request_stored_clients: function()
    {
        let message_object = {
            message:
            {
                type: "request_stored_clients",
            }
        };

        send_message_object(message_object);
    },

    // asks the server to bring the local client back from idle into the given channel
    send_come_from_idle_mode_request: function(channelId = null)
    {
        let message_object = {
            message:
            {
                type: "come_back_from_idle_mode_request",
                channel_id: channelId,
            }
        };

        send_message_object(message_object);
    },

    // asks the server to rename the given client to the given username
    send_change_client_username_request: function(username_to_set, client_id)
    {
        let message_object = {
            message: {
                type: "change_client_username",
                client_id: client_id,
                new_username: username_to_set
            }
        };

        send_message_object(message_object);
    },

    // requests the song list of the currently selected musicbot (selected_client_id)
    send_musicbot_song_list_request: function()
    {
        let message_object = {
            message:
            {
                type: "musicbot_get_song_list",
                musicbot_id: parseInt(selected_client_id),
            }
        };

        send_message_object(message_object);
    },

    // uploads one base64 chunk of a file to the server; index 0 flags the start
    // of a new file (also sets the global is_new_file)
    send_file_send_request: function(total_bytes_length, data_part_base64, index)
    {
        // uploads file by smaller base64 parts that server will merge together to produce file... for now this will be used only for mp3 files
        // after the file upload is done, server sends the information to the client that it is done, and by that it asks the client what to do with the file (is it mp3 file, image for channel, for client, or avatar?)

        is_new_file = false;
        if (index === 0)
        {
            is_new_file = true;
        }

        let message_object = {
            message: {
                type: "file_send",
                total_bytes_length: total_bytes_length,
                data_part_base64: data_part_base64,
                is_new_file: is_new_file
            }
        };

        send_message_object(message_object);
    },
    // tells the server what the fully uploaded file is for (the intent argument
    // plus the global file_send_intent_extra_data)
    send_file_send_completed_request: function(file_send_intent)
    {
        // after file upload is done, server informs the client that it is done. Clients then tells the server what to do with the file (file_send_intent)

        let message_object = {
            message: {
                type: "file_send_completed",
                file_send_intent: file_send_intent,
                file_send_intent_extra_data: file_send_intent_extra_data
            }
        };

        send_message_object(message_object);
    },

    // asks the given musicbot to delete the song with the given id
    send_remove_song_from_music_bot_request: function(song_id, selected_client_id) {
        let message_object = {
                message: {
                    type: "remove_song_from_music_bot",
                    musicbot_id: selected_client_id,
                    song_id: song_id
                }
            };

        send_message_object(message_object);
    }
};
