// messages.js is embedded in template.html along with the other client files, and in the node bundle
// it is the protocol layer: server_msg holds a handler for every server->client message, client_msg the
// builders for client->server requests; the message format details are here, not in the ui
// dispatch.js calls the handlers, ui.js and the feature files call the builders

// state private to this file
var received_files = []; // in chunks

var is_long_press = null;

var server_msg = {
    /**
     * @brief Applies the permission values announced by the server.
     * @param msg -> changed group and its permissions
     */
    process_group_permissions_changed: function(msg)
    {
        let group = channel_tree__get_group_by_group_id(msg.message.group_id);
        if (group)
        {
            group.permissions = msg.message.permissions;
        }
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
    },
    /**
     * @brief Updates the group list and the groups beside connected users.
     *
     * @param object msg -> the server message, msg.message.groups holds the groups
     *
     * @return void
     */
    process_group_list_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        UI.wire_settings_delete_delegation();

        g_groups = msg.message.groups.slice();
        if (msg.message.access)
        {
            groups__apply_access(msg.message.access);
        }
        for (let client of g_client_list)
        {
            let holder = document.getElementById("client-groups-" + client.client_id);
            if (holder)
            {
                holder.replaceChildren();
            }
        }

        // add groups for clients in UI

        for (let i = 0; i < g_client_list.length; i++)
        {
            console.log("process_group_list_from_server | processing client: " + g_client_list[i].username);
            for (let y = 0; y < g_client_list[i].group_ids.length; y++)
            {
                console.log("process_group_list_from_server | processing group id : " + g_client_list[i].group_ids[y]);

                let group = channel_tree__get_group_by_group_id(g_client_list[i].group_ids[y]);

                if (group == null)
                {
                    console.log("group is null");
                    continue;
                }

                let target_element = document.getElementById("client-groups-" + g_client_list[i].client_id);

                // clients in a hidden channel have no row, and throwing here aborted the loop
                // so every client after them lost their groups too
                if (target_element == null)
                {
                    continue;
                }

                const node = document.createElement("div");
                node.className = "single-group";
                node.setAttribute("group-id", group.group_id);

                let icon = group.has_icon ? channel_tree__get_icon_by_icon_id(group.group_linked_icon_id) : null;

                if (icon != null)
                {
                    node.style.backgroundImage = "url("+icon.base64_icon+")";
                }

                target_element.appendChild(node);
            }
        }
    },
    /**
     * @brief Updates the saved users shown beside the selected server group.
     * @param msg -> identities sent by the server
     */
    process_identity_list_from_server: function(msg)
    {
        if (!g_is_authenticated || !groups__has_permission("view_group_members"))
        {
            return;
        }
        groups_window.identities = Array.isArray(msg.message.identities) ? msg.message.identities : [];
        groups__schedule_refresh();
    },
    /**
     * @brief Replaces the server icon list and refreshes icons shown in channels and groups.
     *
     * @param object msg -> the server message, msg.message.icons holds the icons
     *
     * @return void
     */
    process_icon_list_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        UI.wire_settings_delete_delegation();

        g_icons.length = 0;

        for (let i = 0; i < msg.message.icons.length; i++)
        {
            let icon = {
                id: msg.message.icons[i].icon_id,
                base64_icon: msg.message.icons[i].base64_icon
            }

            g_icons.push(icon);


        }

        // g_channel_list arrives BEFORE this icon_list, so channel rows were rendered with empty icon
        // boxes; now that the icons exist, paint them
        UI.refresh_all_channel_icons();
    },
    /**
     * @brief the channel list, once per session: builds the whole channel tree html from the server snapshot, fills g_channel_list, wires the click and touch handlers, sets g_is_channel_list_retrieved
     *
     * @param object msg -> the server message, msg.message.channels holds the channels
     *
     * @return void
     */
    process_channel_list_from_server: function(msg)
    {
        if (g_is_channel_list_retrieved == true)
        {
            utils__custom_log("channel_list message received more than once. Server is doing something weird");
            return;
        }

        g_is_channel_list_retrieved = true;
        document.getElementById("channel-list-container").innerHTML = "";
        var root_channels = channel_tree__get_channels_by_channel_parent_id(msg.message.channels, g_ROOT_LEVEL_PARENT_MARKER);

        let processed_channels = [];
        let current_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_MARKER;
        let previous_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_MARKER;
        let channel_tree_build_stall_counter = 0;

        while (processed_channels.length != msg.message.channels.length)
        {
            let processed_count_before_iteration = processed_channels.length;

            if (current_parent_channel_id_to_find == previous_parent_channel_id_to_find)
            {
                current_parent_channel_id_to_find = g_ROOT_LEVEL_PARENT_MARKER;
            }
            previous_parent_channel_id_to_find = current_parent_channel_id_to_find;

            var child_channels = channel_tree__get_channels_by_channel_parent_id(msg.message.channels, current_parent_channel_id_to_find);

            for (let a = 0; a < child_channels.length; a++)
            {
                // first, check if current child channel is present in HTML. That is done by checking its presence in g_channel_list array.
                // if not, add current channel to g_channel_list and append channel to HTML

                let is_channel_added_to_html = channel_tree__get_channel_by_id(g_channel_list, child_channels[a].channel_id);

                if (is_channel_added_to_html == null)
                {
                    child_channels[a].is_channel_directly_collapsed = false;
                    g_channel_list.push(child_channels[a]);

                    let indentation_level = channel_tree__get_indentation_level(child_channels[a].channel_id, msg.message.channels);

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
                                            <p class='single-channel-name-p "+html_to_append_is_using_password_class+" "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + child_channels[a].channel_id + "\">" + chat__sanitize_string(child_channels[a].name) + "</p>\n\
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
                                                <p class='single-channel-name-p "+html_to_append_is_temp_class+"' data-channel-name-id=\"" + child_channels[a].channel_id + "\" >" + chat__sanitize_string(child_channels[a].name) + "</p>\n\
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
                        if (current_parent_channel_id_to_find == g_ROOT_LEVEL_PARENT_MARKER)
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

                    var children_of_child_channel = channel_tree__get_channels_by_channel_parent_id(msg.message.channels, child_channels[a].channel_id);
                    if (children_of_child_channel.length > 0)
                    {
                        current_parent_channel_id_to_find = child_channels[a].channel_id;
                        break;
                    }

                    processed_channels.push(child_channels[a].channel_id);
                }
            }

            // safety check: a correct pass always processes at least one channel.
            // if an entire pass made no progress, the channel data is inconsistent
            // (missing root, a child without a parent, or a cycle) - stop instead of freezing the UI.
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

        // handle click events on channels differently on touch devices
        if (g_is_client_running_under_touch_device)
        {
            for (let i = 0; i < elements.length; i++)
            {
                if (elements[i].classList.contains("idle-channel"))
                {
                    continue;
                }

                UI.setup_channel_touch_events(elements[i]);

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
                console.log("selected_channel_id1 => " + selected_channel_id1);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("dblclick", UI.single_channel_doubleclick_join);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("click", UI.single_channel_onclick);
                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }

        // recompute chevron (leaf) visibility after the channel tree changed
        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief a client stopped streaming a song: hides the marquee on their row; for the local client also sets g_stop_song_stream_message_received so the song stops being sent
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
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

        if (msg.message.client_id == g_local_client_id)
        {
            g_stop_song_stream_message_received = true;
        }
    },
    /**
     * @brief video_stream_state: a stream started or ended in a channel (see video.js)
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_video_stream_state_from_server: function(msg)
    {
        video__on_state(msg);
    },
    /**
     * @brief video_stream_offer: the channel's stream is offered to this client, it may press connect
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_video_stream_offer_from_server: function(msg)
    {
        video__on_offer(msg);
    },
    /**
     * @brief video_stream_viewer_request: a member joined our streaming channel, may they watch?
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_video_stream_viewer_request_from_server: function(msg)
    {
        video__on_viewer_request(msg);
    },
    /**
     * @brief video_stream_viewer_state: a viewer of our stream connected or dropped
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_video_stream_viewer_state_from_server: function(msg)
    {
        video__on_viewer_state(msg);
    },
    /**
     * @brief video_stream_keyframe_request: a viewer of our stream lost frames
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_video_stream_keyframe_request_from_server: function(msg)
    {
        video__on_keyframe_request();
    },
    /**
     * @brief video_stream_refused: the server said no to a stream start or a connect
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_video_stream_refused_from_server: function(msg)
    {
        video__on_refused(msg);
    },
    /**
     * @brief somebody connected: adds them to g_client_list (and the id map), renders their row with handlers, promotes their offline chat, re-keys the root channel if we maintain it, and requests their avatar
     *
     * @param object msg -> the server message, msg.message holds the client's fields
     *
     * @return void
     */
    process_client_connect_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let local_client = channel_tree__get_client_by_client_id(g_local_client_id);
        if (!g_is_authenticated || local_client == null)
        {
            return;
        }
        let username = chat__sanitize_string(msg.message.username);
        let client_id = msg.message.client_id;

        if (local_client.channel_id == msg.message.channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_entered_your_channel.play();
            }
        }
        let single_client = {
            client_id: null,
            username: null,
            identity_hash: null,
            public_key: null,
            channel_id: null,
            audio_state: null,
            group_ids: null,
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
        single_client.identity_hash = (msg.message.identity_hash != null) ? msg.message.identity_hash : "";
        single_client.public_key = msg.message.public_key;
        single_client.channel_id = msg.message.channel_id;
        single_client.group_ids = (msg.message.group_ids != null) ? msg.message.group_ids : [];
        single_client.audio_state = msg.message.audio_state;
        single_client.is_clients_channel_hidden = false;
        single_client.country_iso_code = msg.message.country_iso_code;
        single_client.is_idle = false;
        single_client.is_ignored_by_local_client = false;
        single_client.is_muted_by_local_client = false;
        single_client.unread_count = 0;
        single_client.is_music_bot = msg.message.is_music_bot;
        single_client.music_bot_encryption = msg.message.music_bot_encryption || 0;
        single_client.music_bot_keys = msg.message.music_bot_keys || null;

        channel_tree__forget_client(single_client.client_id);
        g_client_list.push(single_client);
        g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
        keys__channel_member_joined(single_client);

        // an offline conversation with this person becomes their live private chat, history kept;
        // the merge is state and happens here, only the markup move is left to the ui
        let promotion = chat__promote_offline_chat_context_state(single_client);
        UI.promote_offline_chat_context_render(promotion, single_client);

        html_to_append = channel_tree__generate_html_for_single_client(single_client, false);

        channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

        let elements = document.querySelectorAll('.connected-client[data-connected-client-id="' + single_client.client_id + '"]');

        // local user uses diferent handler
        for (let i = 0; i < elements.length; i++)
        {

            if (g_is_client_running_under_touch_device)
            {

                let local_touch_press_timer = null; // for touch devices

                elements[i].addEventListener("touchstart", (event) => {

                    is_long_press = false;

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    local_touch_press_timer = window.setTimeout( () => {

                        is_long_press = true;
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {

                    clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                    const currentTarget = event.currentTarget;
                    const clientY = event.changedTouches[0].pageY;
                    const clientX = event.changedTouches[0].pageX;

                    if (is_long_press == false)
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

        // add group ids

        console.log("process_group_list_from_server | processing client: " +  single_client.username);
        for (let y = 0; y < single_client.group_ids.length; y++)
        {
            console.log("process_group_list_from_server | processing group id : " + single_client.group_ids[y]);

            let group = channel_tree__get_group_by_group_id(single_client.group_ids[y]);

            if (group == null)
            {
                console.log("group is null");
                continue;
            }

            let target_element = document.getElementById("client-groups-" + single_client.client_id);

            const node = document.createElement("div");
            node.className = "single-group";
            node.setAttribute("group-id", group.group_id);

            // has_icon is the only thing saying an icon was assigned, and id 0 resolves to a real
            // icon, so an unguarded lookup paints one on groups that have none
            let icon = (group.has_icon == true) ? channel_tree__get_icon_by_icon_id(group.group_linked_icon_id) : null;

            if (icon != null)
            {
                node.style.backgroundImage = "url("+icon.base64_icon+")";
            }

            target_element.appendChild(node);
        }

        UI.refresh_all_channel_fullness();

        // a new client joined: pull their avatar (if any) so it shows on their row
        channel_tree__request_single_avatar(client_id);

        // a bot that just appeared streams within seconds; the player must already know it is one
        // (the headless node build has no player: audio.js is not part of its bundle)
        if (typeof audio__audio_player_announce_music_bots === "function")
        {
            audio__audio_player_announce_music_bots();
        }
    },
    /**
     * @brief Removes a disconnected client and refreshes the saved contact list.
     * @param msg Server message naming the disconnected client.
     */
    process_client_disconnect_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let disconnecting_client = channel_tree__get_client_by_client_id(msg.message.client_id);
        let local_client = channel_tree__get_client_by_client_id(g_local_client_id);

        // Remove the user before updating chat panels or playing a sound.
        // A missing panel must not leave a disconnected user in the channel list.
        channel_tree__forget_client(msg.message.client_id);
        keys__channel_member_left(disconnecting_client);
        chat__clear_typing_from_client(msg.message.client_id);

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

        groups__schedule_stored_users_refresh();

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        if (document.getElementById('chat-context-pm-' + msg.message.client_id) != null)
        {
            document.getElementById('chat-context-pm-' + msg.message.client_id).remove();
        }

        let chat_context_index_to_remove = chat__get_chat_context_index_by_chat_context_id("chat-context-pm-" + msg.message.client_id);

        if (chat_context_index_to_remove != -1)
        {
            g_chat_context_array.splice(chat_context_index_to_remove, 1);
        }

        // if used chat context was deleted one, switch to current channel
        if (g_current_chat_context_id == "chat-context-pm-" + msg.message.client_id)
        {
            console.log("switching to main channel? ");
            g_current_chat_context_id = "chat-context-channel-" + g_current_channel_id;
            chat_replies__refresh();
        chat__clear_channel_unread_count(g_current_channel_id); // opened it, so it is read
            document.getElementById("chat-context-channel-" + g_current_channel_id).style.display = "block";
            g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_identity_hash = ""; // back on a channel: no offline target
            UI.schedule_member_list_sync(); // active ring moves back to the channel circle
        }

        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief an incoming call, on its way to the native accept/decline screen: the Android object when a webview exists, the bridge listener when node runs headless
     *
     * @param object msg -> the server message, msg.message.caller_client_id names the caller
     *
     * @return void
     */
    process_call_from_server: function(msg)
    {
        let client = channel_tree__get_client_by_client_id(msg.message.caller_client_id);

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

        utils__custom_alert("incoming call from " + chat__sanitize_string(client.username));
    },
    /**
     * @brief a client left idle: updates their channel_id and is_idle, re-renders their row in the target channel with handlers, plays the join sound and may resume the always-on mic
     *        for the local client the channel globals are synced first, or the maintainer_id message
     *        (and with it the channel keys) would be silently discarded when returning straight into a call
     *
     * @param object msg -> the server message, msg.message holds client_id and channel_id
     *
     * @return void
     */
    process_client_coming_back_from_idle_mode_from_server: function(msg)
    {
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].channel_id = msg.message.channel_id;
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = false;

        // the local client returned from idle: sync the channel globals - a stale
        // current_channel_id made the maintainer_id message (and with it the channel
        // keys) be silently discarded when returning straight into a call
        if (msg.message.client_id == g_local_client_id)
        {
            g_current_channel_id = msg.message.channel_id;
            groups__refresh_channel_controls();
            video__on_local_channel_changed();
            keys__set_current_channel_keys(null);

            // the server answered, so we are allowed to go idle again
            android_host__clear_come_from_idle_in_flight();
        }

        keys__channel_member_joined(channel_tree__get_client_by_client_id(msg.message.client_id));

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        let single_client = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];

        if (g_local_client_id != single_client.client_id)
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, false);
        }
        else
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, true);
        }

        channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);

        let element = document.querySelector('.connected-client[data-connected-client-id="'+msg.message.client_id+'"]');

        if (g_is_client_running_under_touch_device)
        {
            let local_touch_press_timer = null; // for touch devices

            element.addEventListener("touchstart", (event) => {

                is_long_press = false;

                // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                // by the time setTimeout runs, event object (or at least part of it) is lost,
                // things from event object must be imediatelly stored in temp variable in this case, for later use

                const currentTarget = event.currentTarget;
                const clientY = event.touches[0].clientY;
                const clientX = event.touches[0].clientX;

                local_touch_press_timer = window.setTimeout( () => {

                    is_long_press = true;
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                },
                600, event);

            });

            element.addEventListener("touchend", (event) => {

                clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                const currentTarget = event.currentTarget;
                const clientY = event.changedTouches[0].pageY;
                const clientX = event.changedTouches[0].pageX;

                if (is_long_press == false)
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
        let local_client_channel_id = channel_tree__get_client_by_client_id(g_local_client_id).channel_id;

        if (local_client_channel_id == msg.message.channel_id && g_local_client_id != msg.message.client_id)
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
    /**
     * @brief the server's sdp offer for the datachannel: applied to g_peer_connection_with_server, then an answer is created, set as local description and sent back
     *        node sees this frame too (it owns the socket) but never built a peer connection, so it just forwards and ignores
     *
     * @param object msg -> the server message, msg.message.value is the offer as json
     *
     * @return void
     */
    process_sdp_offer_from_server: function(msg)
    {
        // voice runs in the webview only. node sees this frame too (it owns the socket)
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
                        voice__send_sdp_answer_to_server(answer);
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
    /**
     * @brief a channel was deleted: removes it from g_channel_list and the dom; plays the delete sound when the local client requested it
     *
     * @param object msg -> the server message, msg.message holds channel_id and channel_deletor_id
     *
     * @return void
     */
    process_channel_delete_from_server: function(msg)
    {
        console.log("channel deleted: channel_id=" + msg.message.channel_id);
        if (Number.isInteger(msg.message.channel_deletor_id) && msg.message.channel_deletor_id === g_local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_deleted.play();
            }
        }

        let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        if (channel_index != -1)
        {
            g_channel_list.splice(channel_index, 1);
            document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').remove();
            UI.refresh_all_channel_fullness();
        }
    },
    /**
     * @brief an ice candidate from the server, fed into g_peer_connection_with_server; nothing without one (node, or a candidate arriving just after a teardown)
     *
     * @param object msg -> the server message, msg.message.value is the candidate
     *
     * @return void
     */
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
    /**
     * @brief a server broadcast: appended to the root channel chat context, the trailing spacer rebuilt, the chat scrolled to the bottom
     *
     * @param object msg -> the server message, msg.message.value is the text
     *
     * @return void
     */
    process_server_info_broadcast_from_server: function(msg)
    {
        let index = chat__get_chat_context_index_by_chat_context_id("chat-context-channel-0");
        console.log("process_server_info_broadcast_from_server chat context index -> " + index);
        g_chat_context_array[index].last_known_message_sender_username = "";
        let html_to_append = "<div class=\"single-server-message\">" + chat__sanitize_string(msg.message.value) + " " + new Date().toLocaleTimeString() + "</div>";
        document.getElementById("chat-context-channel-0").insertAdjacentHTML("beforeend", html_to_append);
        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append1 = "<div class=\"chat-spacer\"></div>";
        document.getElementById("chat-context-channel-0").insertAdjacentHTML("beforeend", html_to_append1);
        chat__scroll_chat_to_end(false);
    },
    /**
     * @brief Records the delivered message identifier and enables its available actions.
     *
     * @param object msg -> the server message, msg.message holds local_message_id and server_chat_message_id
     *
     * @return void
     */
    process_server_chat_message_id_for_local_message_id_from_server: function(msg)
    {
        // A late acknowledgement must not keep author data for a message already removed locally.
        if (document.querySelector('[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]') == null)
        {
            return;
        }
        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = g_rsa_public_key_string;

        let element = document.querySelector('.local-single-chat-message-content-p[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

        if (element != null)
        {
            element.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
            let state = chat_reactions.entries.get(element);
            if (state)
            {
                chat_reactions__update_message_access(state);
            }
        }
        else
        {
            let element = document.querySelector('.local-client-chat-picture-img[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

            if (element != null)
            {
                element.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
                chat_reactions__register_attachment(element);
            }
            else
            {
                let file_card = document.querySelector('.local-client-chat-file[data-single-chat-message-local-message-id="' + msg.message.local_message_id + '"]');

                if (file_card != null)
                {
                    file_card.setAttribute("data-single-chat-message-server-message-id", msg.message.server_chat_message_id);
                    chat_reactions__register_attachment(file_card);
                }
            }
        }
    },
    /**
     * @brief a channel was edited: applies the fields to g_channel_list and its row (name text plus the password and audio-disabled classes); plays the edited sound if the local client edited it
     *
     * @param object msg -> the server message, msg.message holds the channel's fields and channel_editor_id
     *
     * @return void
     */
    process_channel_edit_from_server: function(msg)
    {
        console.log("channel edited: channel_id=" + msg.message.channel_id);
        if (msg.message.channel_editor_id == g_local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.channel_edited.play();
            }
        }

        let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2].innerHTML = chat__sanitize_string(msg.message.channel_name);
        g_channel_list[index].description = msg.message.channel_description;
        g_channel_list[index].is_using_password = msg.message.is_using_password;
        g_channel_list[index].name = msg.message.channel_name;
        g_channel_list[index].is_audio_enabled = msg.message.is_audio_enabled;
        // an older server sends no toggle: the channel then streams (the server side defaults the same way)
        g_channel_list[index].is_video_stream_enabled = (msg.message.is_video_stream_enabled != false);
        video__refresh_controls();
        g_channel_list[index].is_client_limit_active = msg.message.is_client_limit_active;
        g_channel_list[index].max_client_count = msg.message.max_client_count;
        g_channel_list[index].required_talk_power = Number(msg.message.required_talk_power) || 0;
        g_channel_list[index].required_message_power = Number(msg.message.required_message_power) || 0;
        g_channel_list[index].required_picture_power = Number(msg.message.required_picture_power) || 0;
        g_channel_list[index].required_stream_power = Number(msg.message.required_stream_power) || 0;
        groups__refresh_channel_controls();
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
    /**
     * @brief the client-info popup for admins: username, connected time, ip, country, last action, identity
     *
     * @param object msg -> the server message, msg.message holds the info fields
     *
     * @return void
     */
    process_client_info_from_server: function(msg)
    {
        if (!groups__has_permission("view_client_details"))
        {
            return;
        }
        // The server sends these details only to users with permission to view them.
        let client = channel_tree__get_client_by_client_id(msg.message.client_id);
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

    /**
     * @brief a channel was created: pushes it into g_channel_list, inserts its row after the parent's subtree, rewires all channel handlers; plays the created sound if the local client created it
     *
     * @param object msg -> the server message, msg.message holds the channel's fields and channel_creator_id
     *
     * @return void
     */
    process_channel_create_from_server: function(msg)
    {

        console.log(msg);

        if (msg.message.channel_creator_id == g_local_client_id)
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
            is_video_stream_enabled: (msg.message.is_video_stream_enabled != false),
            is_temp_channel: msg.message.is_temp_channel,
            is_client_limit_active: msg.message.is_client_limit_active,
            max_client_count: msg.message.max_client_count,
            required_talk_power: Number(msg.message.required_talk_power) || 0,
            required_message_power: Number(msg.message.required_message_power) || 0,
            required_picture_power: Number(msg.message.required_picture_power) || 0,
            required_stream_power: Number(msg.message.required_stream_power) || 0,
            has_channel_icon: msg.message.has_channel_icon,
            channel_icon_id: msg.message.channel_icon_id,
            is_channel_directly_collapsed: false,
        };

        g_channel_list.push(new_channel);
        let indentation_level = channel_tree__get_indentation_level(new_channel.channel_id, g_channel_list);

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
        let subtree_anchor = channel_tree__get_channel_subtree_last_element(new_channel.parent_channel_id);
        subtree_anchor.insertAdjacentHTML("afterend", html_to_append);

        // paint the new row's icon (a fresh channel has none yet, but this also covers the case
        // where it was created carrying one, and keeps the icon box consistent)
        UI.refresh_channel_icon(new_channel);

        let elements = document.getElementsByClassName('single-channel');

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

                UI.setup_channel_touch_events(elements[i]);

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
                console.log("selected_channel_id1 => " + selected_channel_id1);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("dblclick", UI.single_channel_doubleclick_join);
                elements[i].querySelector('[data-channel-name-id="' + selected_channel_id1 + '"]').addEventListener("click", UI.single_channel_onclick);
                elements[i].getElementsByClassName("single-channel-collapse-button")[0].addEventListener("mousedown", UI.collapse_expand_channel);
            }
        }

        // recompute chevron (leaf) visibility after the channel tree changed
        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief a picture is coming to the channel: appends a placeholder (progress ring) message; the real image arrives later and is matched by picture_id
     *        an ignored sender is skipped
     *
     * @param object msg -> the server message, msg.message holds sender_id, picture_id and size
     *
     * @return void
     */
    process_channel_chat_picture_metadata_from_server: function(msg)
    {
        let client_index = channel_tree__get_client_index_in_array_by_client_id(msg.message.sender_id);

        if (client_index == -1)
        {
            return;
        }

        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        // the ring counts the incoming chunks against the encrypted length the server announced
        chat_files__begin_chat_file_transfer(msg.message.picture_id, msg.message.size);

        let sender_username = g_client_list[client_index].username;
        let index = chat__get_chat_context_index_by_chat_context_id("chat-context-channel-" + g_current_channel_id);
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
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(sender_username)) + "\n\
                                        </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        " + chat_files__generate_chat_picture_progress_html(msg.message.picture_id) + "\n\
                                        <img class='chat-picture-img chat-picture-img-default' data-single-chat-message-server-message-id='"+ msg.message.picture_id + "' id=\"chat-picture-img-" + msg.message.picture_id + "\"></img>\n\
                                    </div>\n\
                                </div>";

        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");

        if (g_current_chat_context_id == receiving_chat_context_id)
        {
            chat__scroll_chat_to_end(false);
        }
    },
    /**
     * @brief a picture is coming in private: creates the pm chat context if missing, plays the received sound, appends the placeholder and bumps the sender's unread badge
     *        g_chat_context_array is the source of truth for the context's existence, not the dom
     *
     * @param object msg -> the server message, msg.message holds sender_id, picture_id and size
     *
     * @return void
     */
    process_direct_chat_picture_metadata_from_server: function(msg)
    {
        // if direct message is of type direct_chat_message, do not display the message if client is ignored
        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        // the ring counts the incoming chunks against the encrypted length the server announced
        chat_files__begin_chat_file_transfer(msg.message.picture_id, msg.message.size);

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
                                            <div class=\"single-server-message\">now talking to user: " + chat__sanitize_string(channel_tree__get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                                <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                                <div class=\"single-server-message\">his public key: " + chat__sanitize_string(channel_tree__get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
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

        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);
        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(msg.message.sender_username)) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\"><p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            " + chat_files__generate_chat_picture_progress_html(msg.message.picture_id) + "\n\
                                            <img class='chat-picture-img-default chat-picture-img' data-single-chat-message-server-message-id='"+ msg.message.picture_id + "' id=\"chat-picture-img-" + msg.message.picture_id + "\"></img>\n\
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

        if (g_current_chat_context_id == received_direct_message_chat_context_id)
        {
            chat__scroll_chat_to_end(false);
        }

        if (g_current_chat_context_id != received_direct_message_chat_context_id)
        {
            chat__increment_unread_count(msg.message.sender_id);
            chat__render_unread_badge(msg.message.sender_id, true);
        }
        g_chat_context_array[chat_context_index].last_known_message_sender_username = msg.message.sender_username;
    },
    /**
     * @brief a file is coming to the channel: the worker already opened its header, so the card goes up now with the name, size, icon and a progress ring; the body fills it in later by file_id
     *
     * @param object msg -> the server message, msg.message holds sender_id, channel_id, file_id, the header and size
     *
     * @return void
     */
    process_channel_chat_file_metadata_from_server: function(msg)
    {
        let client_index = channel_tree__get_client_index_in_array_by_client_id(msg.message.sender_id);

        if (client_index == -1)
        {
            return;
        }

        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(receiving_chat_context_id);

        if (chat_context_index == -1)
        {
            return;
        }

        let sender_username = g_client_list[client_index].username;
        let header = (msg.message.file_header_decrypted != null) ? msg.message.file_header_decrypted : { name: "(file, no key to open it)", size: 0 };

        g_chat_context_array[chat_context_index].last_known_message_sender_username = sender_username;
        g_chat_message_author_public_keys[msg.message.file_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);
        chat_files__begin_chat_file_transfer(msg.message.file_id, msg.message.encrypted_size);

        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(sender_username)) + "\n\
                                        </div>\n\
                                    <div class=\"single-chat-message-sender-time\">\n\
                                        <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                    </div>\n\
                                    <div class=\"single-chat-message-content\">\n\
                                        " + chat_files__generate_chat_file_card_html({ key: msg.message.file_id, name: header.name, size: header.size, is_receiving: true }) + "\n\
                                    </div>\n\
                                </div>";

        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);
        document.getElementById(receiving_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");

        let card = chat_files__get_chat_file_card_by_key(msg.message.file_id);

        if (card != null)
        {
            card.addEventListener("mousedown", chat_files__chat_file_card_onrightclick);
        }

        chat__scroll_chat_to_end(false);
    },
    /**
     * @brief a file is coming in private: creates the pm chat context if missing, plays the received sound, puts up the card with its ring and bumps the sender's unread badge
     *
     * @param object msg -> the server message, msg.message holds sender_id, file_id, the header and size
     *
     * @return void
     */
    process_direct_chat_file_metadata_from_server: function(msg)
    {
        if (channel_tree__get_client_by_client_id(msg.message.sender_id) == null || channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        let received_direct_message_chat_context_id = 'chat-context-pm-' + msg.message.sender_id;
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
            let html_to_append = "<div class=\"chat-context\" id=\"" + received_direct_message_chat_context_id + "\">\n\
                                            <div class=\"single-server-message\">now talking to user: " + chat__sanitize_string(channel_tree__get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                                <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                                <div class=\"single-server-message\">his public key: " + chat__sanitize_string(channel_tree__get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
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

        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);
        let header = (msg.message.file_header_decrypted != null) ? msg.message.file_header_decrypted : { name: "(file, no key to open it)", size: 0 };

        g_chat_message_author_public_keys[msg.message.file_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);
        chat_files__begin_chat_file_transfer(msg.message.file_id, msg.message.encrypted_size);

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(msg.message.sender_username)) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\"><p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            " + chat_files__generate_chat_file_card_html({ key: msg.message.file_id, name: header.name, size: header.size, is_receiving: true }) + "\n\
                                        </div>\n\
                                    </div>\n\
                                </div>";

        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", html_to_append);

        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        document.getElementById(received_direct_message_chat_context_id).insertAdjacentHTML("beforeend", "<div class=\"chat-spacer\"></div>");

        let card = chat_files__get_chat_file_card_by_key(msg.message.file_id);

        if (card != null)
        {
            card.addEventListener("mousedown", chat_files__chat_file_card_onrightclick);
        }

        if (g_current_chat_context_id != received_direct_message_chat_context_id)
        {
            chat__increment_unread_count(msg.message.sender_id);
            chat__render_unread_badge(msg.message.sender_id, true);
        }
        else
        {
            chat__scroll_chat_to_end(false);
        }

        g_chat_context_array[chat_context_index].last_known_message_sender_username = msg.message.sender_username;
    },
    /**
     * @brief the announced maintainer of the current channel, recorded after checking he exists in our channel; we then either send new keys (we are the maintainer) or start the keys-wait timer
     *
     * @param object msg -> the server message, msg.message holds maintainer_id, channel_id and has_maintainer
     *
     * @return void
     */
    process_channel_maintainer_id_from_server: function(msg)
    {
        // A delayed announcement for a channel we left must not change this channel's maintainer.
        if (msg.message.channel_id !== g_current_channel_id
            || !Number.isSafeInteger(msg.message.maintainer_generation) || msg.message.maintainer_generation < 0)
        {
            return;
        }

        // we received some maintainer id from server and we are expected to set it
        // but not so fast, dont let server to trick us...
        // first, check if maintainer really exists at our side and if he really is present in current channel at our end

        let client_index = channel_tree__get_client_index_in_array_by_client_id(msg.message.maintainer_id);

        if (client_index == -1)
        {
            console.log("%c process_channel_maintainer_id_from_server: client with id (" + msg.message.maintainer_id + ") does not exist", "color: red");
            return;
        }

        if (g_client_list[client_index].channel_id != g_current_channel_id)
        {
            console.log("%c process_channel_maintainer_id_from_server: client with id (" + msg.message.maintainer_id + ") not in current channel", "color: red");
            return;
        }

        let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

        // unguarded -1 crashed the whole handler mid-dispatch on device
        if (index == -1)
        {
            console.warn("maintainer message for unknown channel " + g_current_channel_id + ", ignoring");
            return;
        }

        if (g_channel_list[index].maintainer_id !== msg.message.maintainer_id
            || g_channel_list[index].maintainer_generation !== msg.message.maintainer_generation)
        {
            keys__cancel_channel_key_creation();
        }
        g_channel_list[index].maintainer_id = msg.message.maintainer_id;
        g_channel_list[index].maintainer_generation = msg.message.maintainer_generation;
        g_channel_list[index].has_maintainer = msg.message.has_maintainer;
        if (msg.message.has_maintainer && (g_local_client_id == msg.message.maintainer_id) && (g_current_channel_id == msg.message.channel_id))
        {
            // reuse the already-validated index; a fresh unguarded lookup crashed here
            console.log("local user is maintainer of channel '" + g_channel_list[index].name + "'");

            // local user is the key SENDER now, there is nothing to wait for
            keys__cancel_maintainer_keys_wait_timer();

            keys__create_and_send_new_channel_keys();
        }
        else if ((g_current_channel_id == msg.message.channel_id) && (g_local_client_id != msg.message.maintainer_id))
        {
            console.log("%c received channel_maintainer_id " + msg.message.maintainer_id + " for channel: " + msg.message.channel_id, "color: blue");

            // keys from this maintainer are expected shortly; if none arrive within the
            // timeout, the timer votes for a maintainer reset (and keeps re-voting)
            keys__cancel_channel_key_creation();
            keys__arm_maintainer_keys_wait_timer();
        }
        else
        {
            console.log("%c process_channel_maintainer_id_from_server: unknown", "color: blue");
        }
    },
    /**
     * @brief Displays a channel message in its own block with the author and time.
     *
     * @param object msg -> the server message, msg.message holds sender_id, server_chat_message_id and the decrypted payload
     *
     * @return void
     */
    process_channel_chat_message_from_server: function(msg)
    {
        let received_channel_chat_message_object = JSON.parse(msg.message.decrypted_value);
        if (chat_reactions__receive(received_channel_chat_message_object, msg.message, false))
        {
            return;
        }
        chat__clear_typing_from_client(msg.message.sender_id);  // the message arrived, he is done

        if (msg.message.sender_id == g_local_client_id)
        {
            return;
        }

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);

        if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
        {
            return;
        }

        if (g_are_sound_effects_enabled == true)
        {
            g_sound_effects.message_received.play();
        }

        let chat_message_username = chat__sanitize_string(msg.message.sender_username);
        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(receiving_chat_context_id);

        // badge the channel unless it is the one on screen
        chat__increment_channel_unread_count(msg.message.channel_id);

        received_channel_chat_message_object.value = chat__sanitize_string(received_channel_chat_message_object.value);
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

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                        <div class=\"single-message-content\">\n\
                                            <div class=\"single-chat-message-sender-username-container\">\n\
                                                " + android_host__generate_message_sender_html(msg.message.sender_id, chat_message_username) + "\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-sender-time\">\n\
                                                <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-content\">\n\
                                                <p class='single-chat-message-content-p "+chat__sanitize_string(received_channel_chat_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_string(received_channel_chat_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_channel_chat_message_object.value + "</p>\n\
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
        chat__scroll_chat_to_end(false);

        if (g_local_username == chat_message_username)
        {
            document.getElementById("chat-input-container-text-input").value = "";
        }

        if (G_HAS_DOM)
        {
            let context = document.getElementById(receiving_chat_context_id);
            chat_reactions__register(context.querySelector('[data-single-chat-message-server-message-id="' + msg.message.server_chat_message_id + '"]'), received_channel_chat_message_object.reaction_id, channel_tree__get_public_key_by_client_id(msg.message.sender_id), msg.message.sender_username, received_channel_chat_message_object.reply_to);
        }
        g_chat_context_array[chat_context_index].last_known_message_sender_username = chat_message_username;
    },
    /**
     * @brief Displays a queued message in the conversation belonging to the sender identity.
     * @param msg Decrypted message and sender details.
     */
    process_offline_chat_message_from_server: function(msg)
    {
        let sender_name = (typeof msg.message.sender_name === "string") ? msg.message.sender_name : "";
        let sender_identity_hash = msg.message.sender_identity_hash;
        if (typeof sender_identity_hash !== "string" || !/^[A-Za-z0-9+/=]+$/.test(sender_identity_hash))
        {
            return;
        }
        let received_object = null;

        try { received_object = JSON.parse(msg.message.some_json.value); }
        catch (e) { console.warn("offline message payload was not readable:", e.message); return; }

        let message_text = chat__sanitize_string(received_object.value);
        let when_text = (typeof msg.message.queued_unix_seconds === "number" && msg.message.queued_unix_seconds > 0)
            ? UI.format_time_ago(msg.message.queued_unix_seconds)
            : "while you were away";

        let sender_client = null;
        for (let i = 0; i < g_client_list.length; i++)
        {
            if (g_client_list[i] != null && g_client_list[i].identity_hash === sender_identity_hash)
            {
                sender_client = g_client_list[i];
                break;
            }
        }

        let context_id = (sender_client != null) ? ("chat-context-pm-" + sender_client.client_id) : ("chat-context-offline-" + sender_identity_hash);

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
                                    <div class=\"single-server-message\">now talking to user: " + chat__sanitize_string(sender_name) + "</div>\n\
                                </div>";
            document.getElementById('chat-context-container').insertAdjacentHTML("beforeend", html_to_append);

            g_chat_context_array.push({
                type: "user",
                chat_context_id: context_id,
                last_known_message_sender_username: ""
            });

            // a conversation tab so the message is reachable (the member list themes show it through the member list);
            let selector_id = (sender_client != null) ? ("user-" + sender_client.client_id) : ("offline-" + sender_identity_hash);
            if (document.querySelector('[data-chat-context-selector-id="' + selector_id + '"]') == null)
            {
                let selector_html = "<div class=\"chat-context-selector\" data-chat-context-selector-type=\"user\" data-chat-context-selector-id=\"" + selector_id + "\">\n\
                                        <div class=\"p-container\"><p>" + chat__sanitize_string(sender_name) + "</p></div>\n\
                                        <div class=\"remove-chat-context-selector\" data-chat-context-remove-selector-type=\"user\" data-chat-context-remove-selector-id=\"" + selector_id + "\"></div>\n\
                                    </div>";
                document.getElementById("chat-context-selectors-container").insertAdjacentHTML("beforeend", selector_html);

                // freshly inserted markup carries no handlers - rebind them all, the same
                // way every other place that adds a conversation tab does
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
                                        " + android_host__generate_message_sender_html((sender_client != null) ? sender_client.client_id : null, chat__sanitize_string(sender_name)) + "\n\
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
        if (G_HAS_DOM)
        {
            let contact = sender_client || g_offline_client_list.find(function(client)
            {
                return client.identity_hash === sender_identity_hash;
            });
            let element = document.getElementById(context_id).querySelector(".single-chat-message:last-child .single-chat-message-content-p");
            chat_reactions__register(element, received_object.reaction_id, contact && contact.public_key, sender_name, received_object.reply_to);
        }

        if (g_current_chat_context_id == context_id)
        {
            chat__scroll_chat_to_end(false);
        }

        // unread marker on that person, exactly like a live private message
        if (sender_client != null)
        {
            chat__increment_unread_count(sender_client.client_id);
            chat__render_unread_badge(sender_client.client_id, true);
        }

        UI.schedule_member_list_sync();
    },

    /**
     * @brief a direct message, two payload types: renders a private chat message (creating the pm context and the unread badge), or installs the channel keys if they come from the announced maintainer
     *
     * @param object msg -> the server message, msg.message.some_json.type picks the payload
     *
     * @return void
     */
    process_direct_chat_message_from_server: function(msg)
    {
        let received_direct_message_object = msg.message.some_json.type === "direct_chat_message"
            ? JSON.parse(msg.message.some_json.value) : null;
        if (chat_reactions__receive(received_direct_message_object, msg.message, true))
        {
            return;
        }
        chat__clear_typing_from_client(msg.message.sender_id);  // the message arrived, he is done

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = channel_tree__get_public_key_by_client_id(msg.message.sender_id);

        if (msg.message.some_json.type == "direct_chat_message")
        {
            // if direct message is of type direct_chat_message, do not display the message if client is ignored
            if (channel_tree__get_client_by_client_id(msg.message.sender_id).is_ignored_by_local_client == true)
            {
                return;
            }

            received_direct_message_object.value = chat__sanitize_string(received_direct_message_object.value);

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
                                        <div class=\"single-server-message\">now talking to user:  " + chat__sanitize_string(channel_tree__get_display_name_by_client_id(msg.message.sender_id, msg.message.sender_username)) + "</div>\n\
                                        <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                        <div class=\"single-server-message\">his public key: " + chat__sanitize_string(channel_tree__get_public_key_by_client_id(msg.message.sender_id)) + "</div>\n\
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

            let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);

            let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(msg.message.sender_id, chat__sanitize_string(msg.message.sender_username)) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\">\n\
                                            <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            <p class='single-chat-message-content-p "+chat__sanitize_string(received_direct_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_string(received_direct_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_direct_message_object.value + "</p>\n\
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

            if (g_current_chat_context_id == received_direct_message_chat_context_id)
            {
                chat__scroll_chat_to_end(false);
            }
            if (g_current_chat_context_id != received_direct_message_chat_context_id)
            {
                chat__increment_unread_count(msg.message.sender_id);
                chat__remember_message_awaiting_receipt(msg.message.sender_id, msg.message.server_chat_message_id);
                chat__render_unread_badge(msg.message.sender_id, true);
            }
            else
            {
                // the conversation is open. send only if the screen is actually on, otherwise owe it
                if (chat__is_the_user_actually_looking() == true)
                {
                    chat__send_seen_receipt_for_message(msg.message.sender_id, msg.message.server_chat_message_id);
                }
                else
                {
                    chat__remember_message_awaiting_receipt(msg.message.sender_id, msg.message.server_chat_message_id);
                }
            }
            if (G_HAS_DOM)
            {
                let context = document.getElementById(received_direct_message_chat_context_id);
                chat_reactions__register(context.querySelector('[data-single-chat-message-server-message-id="' + msg.message.server_chat_message_id + '"]'), received_direct_message_object.reaction_id, channel_tree__get_public_key_by_client_id(msg.message.sender_id), msg.message.sender_username, received_direct_message_object.reply_to);
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

            chat__mark_message_as_seen(msg.message.sender_id);
        }
        else if (msg.message.some_json.type == "channel_keys_from_maintainer")
        {
            let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

            // unguarded -1 crashed exactly here on device and the keys were never set
            if (index == -1)
            {
                console.warn("channel keys arrived for unknown channel " + g_current_channel_id + ", ignoring");
                return;
            }

            let received_keys = keys__read_channel_key_message(msg.message, g_current_channel_id,
                g_channel_list[index].maintainer_generation);
            if (received_keys == null)
            {
                return;
            }
            if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id == msg.message.sender_id)
            {
                console.log("received keys for channel '" + g_channel_list[index].name + "' from user : '" + msg.message.sender_username + "'. Setting new value of current_channel_keys");
                keys__set_current_channel_keys(received_keys);

                // The decryption thread already installed these keys before processing more messages.
                // Sending this older interface result back could overwrite a newer set in that thread.

                g_opus_decoder_worker.postMessage({
                    type: "mainthread__channel_keys_for_opus_decoder",
                    value: g_current_channel_keys
                });

                // the video worker encrypts and decrypts stream packets with the same keys
                video__post_channel_keys_to_worker();

                // valid keys arrived from the announced maintainer - stop the reset countdown
                keys__cancel_maintainer_keys_wait_timer();

            }
            else
            {
                console.log("received keys for channel '" + g_channel_list[index].name + "' from user : '" + msg.message.sender_username + "' The user is not maintainer of current channel. Refusing to set keys");
                console.log("channel_list[index].maintainer_id = " + g_channel_list[index].maintainer_id);
                console.log("msg.message.sender_id = " + msg.message.sender_id);
            }
        }
    },
    /**
     * @brief a private chat picture arrived: fills its placeholder with the decrypted image and scrolls down
     *
     * @param object msg -> the server message, msg.message holds picture_id and decrypted_base64_picture
     *
     * @return void
     */
    process_direct_chat_picture_from_server: function(msg)
    {
        chat_files__finish_chat_picture_progress(msg.message.picture_id);

        if (document.getElementById("chat-picture-img-" + msg.message.picture_id) != null)
        {
            document.getElementById("chat-picture-img-" + msg.message.picture_id).src = msg.message.decrypted_base64_picture;
            chat__scroll_chat_to_end(false);
            document.getElementById("chat-picture-img-" + msg.message.picture_id).classList.remove("chat-picture-img-default");
        }
    },
    /**
     * @brief a channel chat picture arrived: fills its placeholder with the decrypted image and scrolls down
     *
     * @param object msg -> the server message, msg.message holds picture_id and decrypted_base64_picture
     *
     * @return void
     */
    process_channel_chat_picture_from_server: function(msg)
    {
        chat_files__finish_chat_picture_progress(msg.message.picture_id);

        if (document.getElementById("chat-picture-img-" + msg.message.picture_id) != null)
        {
            document.getElementById("chat-picture-img-" + msg.message.picture_id).src = msg.message.decrypted_base64_picture;
            document.getElementById("chat-picture-img-" + msg.message.picture_id).classList.remove("chat-picture-img-default");
            chat__scroll_chat_to_end(false);
        }
    },
    /**
     * @brief the decrypted body of a private chat file arrived: the card gets its download button
     *
     * @param object msg -> the server message, msg.message holds file_id and the file
     *
     * @return void
     */
    process_direct_chat_file_from_server: function(msg)
    {
        chat_files__finish_chat_file_card(msg.message.file_id, msg.message.file);
    },
    /**
     * @brief the decrypted body of a channel file arrived: the card gets its download button
     *
     * @param object msg -> the server message, msg.message holds file_id and the file
     *
     * @return void
     */
    process_channel_chat_file_from_server: function(msg)
    {
        chat_files__finish_chat_file_card(msg.message.file_id, msg.message.file);
    },
    /**
     * @brief a file body arrived but no key opened it: says so on the card instead of spinning forever
     *
     * @param object msg -> the server message, msg.message.file_id names the transfer
     *
     * @return void
     */
    process_chat_file_decrypt_failed_from_server: function(msg)
    {
        delete g_chat_file_transfers[msg.message.file_id];
        chat_files__mark_chat_file_card_failed(chat_files__get_chat_file_card_by_key(msg.message.file_id), "could not decrypt this file");
    },
    /**
     * @brief the server refused our file and says why: drops the upload lock, tells the user, marks the card
     *
     * @param object msg -> the server message, msg.message holds reason, file_upload_max_size and local_message_id
     *
     * @return void
     */
    process_file_send_error_from_server: function(msg)
    {
        chat__release_file_upload_lock();
        g_file_send_intent = "";
        g_file_send_intent_extra_data = {};

        let text = chat_files__explain_file_send_error(msg.message.reason, msg.message.file_upload_max_size);

        utils__custom_alert(text);
        chat_files__mark_local_chat_file_card_failed(msg.message.local_message_id, text);
    },
    /**
     * @brief our picture upload was relayed: strips the "imgnotsentyet" marker class from every element carrying it
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_image_sent_status_from_server: function(msg)
    {
        chat__hide_picture_delivery_status();

        var lights = document.getElementsByClassName("imgnotsentyet");
        while (lights.length)
        {
            lights[0].classList.remove("imgnotsentyet");
        }
    },
    /**
     * @brief a client went idle: moves them into the idle list (channel_id -2, is_idle true), re-renders their row there with handlers, plays the leave sound and stops our own audio sending
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_client_client_going_to_idle_mode_from_server: function(msg)
    {
        let departing_client = { ...channel_tree__get_client_by_client_id(msg.message.client_id) };
        let client_old_channel_id = channel_tree__get_client_by_client_id(msg.message.client_id).channel_id;
        let local_client_channel_id = channel_tree__get_client_by_client_id(g_local_client_id).channel_id;

        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].channel_id = -2;
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = true;
        keys__channel_member_left(departing_client);

        let single_client = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        if (g_local_client_id != single_client.client_id)
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, false);
        }
        else
        {
            html_to_append = channel_tree__generate_html_for_single_client(single_client, true);
        }

        channel_tree__get_channel_own_clients_last_element("idle").insertAdjacentHTML("afterend", html_to_append);

        let element = document.querySelector('.connected-client[data-connected-client-id="'+msg.message.client_id+'"]');

        if (g_is_client_running_under_touch_device)
        {
            let local_touch_press_timer = null; // for touch devices

            element.addEventListener("touchstart", (event) => {

                is_long_press = false;

                // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                // by the time setTimeout runs, event object (or at least part of it) is lost,
                // things from event object must be imediatelly stored in temp variable in this case, for later use

                const currentTarget = event.currentTarget;
                const clientY = event.touches[0].clientY;
                const clientX = event.touches[0].clientX;

                local_touch_press_timer = window.setTimeout( () => {

                    is_long_press = true;
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                },
                600, event);

            });

            element.addEventListener("touchend", (event) => {

                clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                const currentTarget = event.currentTarget;
                const clientY = event.changedTouches[0].pageY;
                const clientX = event.changedTouches[0].pageX;

                if (is_long_press == false)
                {
                    UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget, true);
                }
            });
        }
        else
        {
            element.addEventListener("mousedown", UI.connected_user_onmousedown);
        }

        if (g_local_client_id != msg.message.client_id && local_client_channel_id == client_old_channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.user_left_your_channel.play();
            }
        }

        // stop sending audio if it was being sent
        voice__process_stop_sending_audio();

    },
    /**
     * @brief a client was renamed: updates the username in g_client_list, on their row (or the local input for ourselves) and on their conversation tab if one exists
     *
     * @param object msg -> the server message, msg.message holds client_id and new_username
     *
     * @return void
     */
    process_client_rename_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let client_index = channel_tree__get_client_index_in_array_by_client_id(parseInt(msg.message.client_id));
        if (client_index == -1)
        {
            return;
        }
        utils__custom_log(g_client_list[client_index].username + ' renamed to -> ' + msg.message.new_username);
        g_client_list[client_index].username = msg.message.new_username;

        if (msg.message.client_id != g_local_client_id)
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
            g_local_username = chat__sanitize_string(msg.message.new_username);
            { let rename_input = document.getElementById("connected-local-client-input"); if (rename_input != null) { rename_input.value = msg.message.new_username; } }
        }

        let is_found = document.querySelector('[data-chat-context-selector-id="user-' + msg.message.client_id + '"]');
        if (is_found != null)
        {
            is_found.getElementsByClassName("p-container")[0].textContent = channel_tree__get_display_name_by_client_id(msg.message.client_id, msg.message.new_username);
        }

        // messages already on screen keep the old name without this
        android_host__retag_rendered_messages_of_sender(msg.message.client_id, channel_tree__get_display_name_by_client_id(msg.message.client_id, msg.message.new_username));
    },
    /**
     * @brief a client's avatar arrived: applied to every ui spot showing that client
     *
     * @param object msg -> the server message, msg.message holds client_id and base64_avatar
     *
     * @return void
     */
    process_client_avatar_from_server: function(msg)
    {
        channel_tree__apply_avatar_to_ui(msg.message.client_id, msg.message.base64_avatar);
    },
    /**
     * @brief a client's avatar changed (no image payload): the cached copy is forgotten and re-fetched
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_avatar_changed_from_server: function(msg)
    {
        if (g_server_policy.is_avatar_upload_allowed == false) { return; }
        let client_object = channel_tree__get_client_by_client_id(msg.message.client_id);
        if (client_object != null) { client_object.base64_avatar = null; }
        channel_tree__request_single_avatar(msg.message.client_id);
    },
    /**
     * @brief the client list, once per session: fills g_client_list (and the id map), identifies the local client, renders every visible row with handlers, then kicks off the avatar lazy-loading
     *
     * @param object msg -> the server message, msg.message.clients holds the clients, local_username names us
     *
     * @return void
     */
    process_client_list_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        if (G_HAS_DOM == false)
        {
            return;
        }

        if (g_is_client_list_retrieved == true)
        {
            utils__custom_log("client_list received more than once. Server is doing something weird");
            return;
        }

        console.log(msg);

        g_is_client_list_retrieved = true;

        for (let i = 0; i < msg.message.clients.length; i++)
        {
            let username = chat__sanitize_string(msg.message.clients[i].username);
            let client_id = msg.message.clients[i].client_id;

            // clients channel is hidden (-1)
            if (msg.message.clients[i].channel_id == -1)
            {
                let single_client = {
                    client_id: null,
                    username: null,
                    identity_hash: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    group_ids: null,
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
                single_client.identity_hash = (msg.message.clients[i].identity_hash != null) ? msg.message.clients[i].identity_hash : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.group_ids = msg.message.clients[i].group_ids;
                single_client.is_clients_channel_hidden = msg.message.clients[i].is_clients_channel_hidden;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;
                single_client.music_bot_encryption = msg.message.clients[i].music_bot_encryption || 0;
                single_client.music_bot_keys = msg.message.clients[i].music_bot_keys || null;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
            }
            else if (msg.message.clients[i].is_idle == true)
            {

                let single_client = {
                    client_id: null,
                    username: null,
                    identity_hash: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    group_ids: null,
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
                single_client.identity_hash = (msg.message.clients[i].identity_hash != null) ? msg.message.clients[i].identity_hash : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.group_ids = msg.message.clients[i].group_ids;
                single_client.is_clients_channel_hidden = msg.message.clients[i].is_clients_channel_hidden;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;
                single_client.music_bot_encryption = msg.message.clients[i].music_bot_encryption || 0;
                single_client.music_bot_keys = msg.message.clients[i].music_bot_keys || null;

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);

                let indentation_level = 1;

                if (username != chat__sanitize_string(msg.message.local_username))
                {
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], false);
                }
                else
                {
                    // this else statement will never run
                    g_local_username = chat__sanitize_string(msg.message.local_username);
                    g_local_client_id = msg.message.clients[i].client_id;
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], true);
                }

                document.querySelector('[data-channel-id="idle"]').insertAdjacentHTML("afterend", html_to_append);
            }
            else
            {
                let html_to_append = "";

                if (username != chat__sanitize_string(msg.message.local_username))
                {
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], false);
                }
                else
                {
                    g_local_username = chat__sanitize_string(msg.message.local_username);
                    g_local_client_id = msg.message.clients[i].client_id;
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], true);
                }

                document.querySelector('[data-channel-id="' + msg.message.clients[i].channel_id + '"]').insertAdjacentHTML("afterend", html_to_append);

                let single_client = {
                    client_id: null,
                    username: null,
                    identity_hash: null,
                    public_key: null,
                    channel_id: null,
                    audio_state: null,
                    group_ids: null,
                    is_clients_channel_hidden: null,
                    country_iso_code: null,
                    is_idle: null,
                    is_ignored_by_local_client: null,
                    is_muted_by_local_client: null,
                    unread_count: 0,
                    is_music_bot: null
                };

                single_client.audio_state = msg.message.clients[i].audio_state;
                single_client.group_ids = msg.message.clients[i].group_ids;
                single_client.is_clients_channel_hidden = false;
                single_client.client_id = parseInt(client_id);
                single_client.username = username;
                single_client.identity_hash = (msg.message.clients[i].identity_hash != null) ? msg.message.clients[i].identity_hash : "";
                single_client.public_key = msg.message.clients[i].public_key;
                single_client.channel_id = msg.message.clients[i].channel_id;
                single_client.country_iso_code = msg.message.clients[i].country_iso_code;
                single_client.is_idle = msg.message.clients[i].is_idle;
                single_client.is_ignored_by_local_client = false;
                single_client.is_muted_by_local_client = false;
                single_client.unread_count = 0;
                single_client.is_music_bot = msg.message.clients[i].is_music_bot;
                single_client.music_bot_encryption = msg.message.clients[i].music_bot_encryption || 0;
                single_client.music_bot_keys = msg.message.clients[i].music_bot_keys || null;

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
                    // .client_id, not .id: the payload has no id field, and a comparison that never
                    // matched would delete every open pm context on every client_list
                    if (g_chat_context_array[x].chat_context_id == "chat-context-pm-" + msg.message.clients[y].client_id)
                    {
                        break;
                    }
                    else
                    {
                        if (y + 1 == msg.message.clients.length)
                        {
                            utils__custom_log("chat context with id " + g_chat_context_array[x].chat_context_id.toString() + " NOT FOUND");
                            document.getElementById(g_chat_context_array[x].chat_context_id).remove();
                        }
                    }
                }
            }
        }

        let elements = document.getElementsByClassName('connected-client');
        for (let i = 0; i < elements.length; i++)
        {
            if (g_is_client_running_under_touch_device)
            {
                let local_touch_press_timer = null; // for touch devices

                elements[i].addEventListener("touchstart", (event) => {

                    is_long_press = false;

                    // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                    // by the time setTimeout runs, event object (or at least part of it) is lost,
                    // things from event object must be imediatelly stored in temp variable in this case, for later use

                    const currentTarget = event.currentTarget;
                    const clientY = event.touches[0].clientY;
                    const clientX = event.touches[0].clientX;

                    local_touch_press_timer = window.setTimeout( () => {

                        is_long_press = true;
                        UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                    },
                    600, event);

                });

                elements[i].addEventListener("touchend", (event) => {

                    clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                    const currentTarget = event.currentTarget;
                    const clientY = event.changedTouches[0].pageY;
                    const clientX = event.changedTouches[0].pageX;

                    if (is_long_press == false)
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
        channel_tree__enqueue_all_avatars_for_loading();

        // themes that do not show the avatar grid get the paced one-at-a-time prefetch instead
        channel_tree__start_avatar_prefetch();

        // the player needs to know which senders are bots before their first audio arrives
        // (the headless node build has no player: audio.js is not part of its bundle)
        if (typeof audio__audio_player_announce_music_bots === "function")
        {
            audio__audio_player_announce_music_bots();
        }

        // a stream already running when the list arrives: its marquee shows when the streamer is in
        // this channel, or wherever it is when the server shows marquees to everyone
        for (let i = 0; i < msg.message.clients.length; i++)
        {
            let listed = msg.message.clients[i];

            if (listed.is_streaming_song != true) { continue; }
            if (g_server_policy.is_music_bot_marquee_visible_to_everyone == false && listed.channel_id != g_current_channel_id) { continue; }

            server_msg.process_start_song_stream_from_server({ message: { client_id: listed.client_id, song_name: listed.song_name } });
        }

        // a video stream already running in our channel: its icon appears; the offer, if we may watch, comes separately
        video__apply_client_list(msg.message.clients);

        // a saved member list theme is applied before the server policy allowed avatars, so the grid never
        // started; re-evaluate now that the flag and the clients exist (the refresh bulk-enqueues itself)
        UI.refresh_member_list_state();
    },
    /**
     * @brief the server denied a request: shows its reason when it sent one, the generic line otherwise
     *
     * @param object msg -> the server message, msg.message.reason is the optional text
     *
     * @return void
     */
    process_access_denied_from_server: function(msg)
    {
        utils__custom_alert((typeof msg.message.reason === "string" && msg.message.reason.length > 0)
            ? chat__sanitize_string(msg.message.reason) : "insufficient permissions");
        g_sound_effects.insufficient_permissions.play();
    },
    /**
     * @brief the admin log: fills the log tab's textarea, one dated line per row, newest at the bottom
     *
     * @param object msg -> the server message, msg.message.lines holds the lines
     *
     * @return void
     */
    process_admin_log_from_server: function(msg)
    {
        if (!groups__has_permission("view_server_logs"))
        {
            return;
        }
        document.getElementById("groups-log-text").value = Array.isArray(msg.message.lines) ? msg.message.lines.join("\n") : "";
        let textarea = document.getElementById("server-settings-log-textarea");
        let lines = Array.isArray(msg.message.lines) ? msg.message.lines : [];

        textarea.value = lines.join("\n");
        textarea.scrollTop = textarea.scrollHeight;
    },
    /**
     * @brief somebody switched channel: moves their row, replays their group labels, plays the join, leave or switch sound; for the local client also swaps the chat context and nulls the channel keys
     *
     * @param object msg -> the server message, msg.message holds client_id and channel_id (-1 for a hidden channel)
     *
     * @return void
     */
    process_channel_join_from_server: function(msg)
    {
        let client_that_joined = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];
        let client_old_channel_id = client_that_joined.channel_id;
        let departing_client = { ...client_that_joined };
        client_that_joined.channel_id = msg.message.channel_id;
        if (client_old_channel_id !== msg.message.channel_id)
        {
            keys__channel_member_left(departing_client);
        }
        keys__channel_member_joined(client_that_joined);

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
            let local_client_channel_id = channel_tree__get_client_by_client_id(g_local_client_id).channel_id;

            if (local_client_channel_id == msg.message.channel_id && g_local_client_id != msg.message.client_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.user_joined_your_channel.play();
                }
            }
            else if (g_local_client_id == msg.message.client_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.channel_switched.play();
                }
            }
            else if (g_local_client_id != msg.message.client_id && local_client_channel_id == client_old_channel_id)
            {
                if (g_are_sound_effects_enabled)
                {
                    g_sound_effects.user_left_your_channel.play();
                }
            }

            if (msg.message.client_id == g_local_client_id)
            {
                audio__audio_player_clear();

                // leaving a channel drops its marquees, unless the server shows them to everyone
                if (g_server_policy.is_music_bot_marquee_visible_to_everyone == false)
                {
                    let marquee_containers = document.getElementsByClassName("marquee-music-playing-container");
                    for (let i = 0; i < marquee_containers.length; i++)
                    {
                        marquee_containers[i].style.display = "none";
                    }
                }

                document.getElementById("channel-password-enter-container").style.display = "none";
                document.getElementById("background-container").style.display = "none";
                document.getElementsByClassName("connected-local-client")[0].remove();

                html_to_append = channel_tree__generate_html_for_single_client(client_that_joined, true);

                channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);
                { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout); } }

                keys__cancel_channel_key_creation();
                g_current_channel_id = msg.message.channel_id;
                groups__refresh_channel_controls();
            video__on_local_channel_changed();
                keys__set_current_channel_keys(null);

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
                    // context label depend on the row having been painted first. chat__sanitize_string
                    // here yields exactly what that markup already held, so the text is unchanged
                    let joined_channel = channel_tree__get_channel_by_id(g_channel_list, msg.message.channel_id);
                    let channel_name = (joined_channel != null) ? chat__sanitize_string(joined_channel.name) : "";

                    let html_to_append3 = '<div class="chat-context" id="chat-context-channel-' + msg.message.channel_id + '" style="display: none;">\n\
                                                <div class="single-server-message">now talking channel: ' + channel_name + '</div>\n\
                                                    </div>\n\
                                                </div>\n\
                                            </div>';

                    // the sibling insert is kept rather than switched to a container append, so
                    // the node ends up in the same place in the browser. only the blind [count - 1]
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
                chat_replies__refresh();
        chat__clear_channel_unread_count(msg.message.channel_id); // opened it, so it is read
                g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_identity_hash = ""; // back on a channel: no offline target
                console.log("joined new channel, nulling out current_channel_keys");
                keys__set_current_channel_keys(null);

                // joined new channel, now loop through clients and set microphone state to 2 for all clients that have it set to 1
                // only clients in current channel should have microphone state set to 1
                // information about active mic state of clients in new channel is later received through websocket and processed

                for (var i = 0; i < g_client_list.length; i++)
                {
                    if (g_client_list[i].audio_state == G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
                    {
                        g_client_list[i].audio_state = G_AUDIO_STATE.PUSH_TO_TALK_ENABLED; // not active but enabled

                        let client = {
                            client_id: g_client_list[i].client_id,
                            audio_state: g_client_list[i].audio_state
                        };

                        voice__process_audio_state_of_single_client(client);
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

                html_to_append = channel_tree__generate_html_for_single_client(client_that_joined, false);

                channel_tree__get_channel_own_clients_last_element(msg.message.channel_id).insertAdjacentHTML("afterend", html_to_append);


            }

            // html element that represents client is now appended to newly joined channel at this point
            // now append client groups to client again

            for (let y = 0; y < client_that_joined.group_ids.length; y++)
            {
                let group = channel_tree__get_group_by_group_id(client_that_joined.group_ids[y]);
                if (group == null)
                {
                    console.log("group is null");
                    continue;
                }
                let target_element = document.getElementById("client-groups-" + client_that_joined.client_id);
                const node = document.createElement("div");
                node.className = "single-group";
                node.setAttribute("group-id", group.group_id);

                // same guard as everywhere else: this is the path that runs when somebody switches
                // channel, which is why an unassigned icon used to appear only after a channel change
                let icon = (group.has_icon == true) ? channel_tree__get_icon_by_icon_id(group.group_linked_icon_id) : null;
                if (icon != null)
                {
                    node.style.backgroundImage = "url("+icon.base64_icon+")";
                }
                target_element.appendChild(node);
            }

            let elements = document.getElementsByClassName('connected-client');

            for (let i = 0; i < elements.length; i++)
            {
                if (g_is_client_running_under_touch_device)
                {
                    let local_touch_press_timer = null; // for touch devices

                    elements[i].addEventListener("touchstart", (event) => {

                        is_long_press = false;

                        // in most modern browsers, event.currentTarget is null after handler exits, especially after longer delay
                        // by the time setTimeout runs, event object (or at least part of it) is lost,
                        // things from event object must be imediatelly stored in temp variable in this case, for later use

                        const currentTarget = event.currentTarget;
                        const clientY = event.touches[0].clientY;
                        const clientX = event.touches[0].clientX;

                        local_touch_press_timer = window.setTimeout( () => {

                            is_long_press = true;
                            UI.connected_user_onmousedown(event, true, clientX, clientY, currentTarget);
                        },
                        600, event);

                    });

                    elements[i].addEventListener("touchend", (event) => {

                        clearTimeout(local_touch_press_timer); // clear the press timer, so the setTimeout doesnt get triggered and long press isnt run if it was goign to be

                        const currentTarget = event.currentTarget;
                        const clientY = event.changedTouches[0].pageY;
                        const clientX = event.changedTouches[0].pageX;

                        if (is_long_press == false)
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
                    document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = chat__sanitize_string(msg.message.song_name);
                }
                else
                {
                    console.log("could not find element");
                }
            }
        }

        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief a chat message was deleted: blanked to "deleted" (a picture reset to the placeholder) after checking the requester is the recorded author or an admin
     *
     * @param object msg -> the server message, msg.message holds chat_message_id, requester_public_key and requester_is_admin
     *
     * @return void
     */
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
            chat_reactions__forget(element, true);
            element.innerHTML = "deleted";
            element.classList.remove("chat-message-edited");
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
                chat_reactions__forget(element, true);
                element.src = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIzMiIgaGVpZ2h0PSIzMiIgdmlld0JveD0iMCAwIDI0IDI0Ij48ZyBzdHJva2UtbGluZWNhcD0icm91bmQiIHN0cm9rZS1saW5lam9pbj0icm91bmQiPjxyZWN0IHg9IjMuNSIgeT0iMy41IiB3aWR0aD0iMTciIGhlaWdodD0iMTciIHJ4PSIyIiBmaWxsPSIjMjIzMTNhIiBzdHJva2U9IiNhZWJiYzUiIHN0cm9rZS13aWR0aD0iMS40Ii8+PGNpcmNsZSBjeD0iOCIgY3k9IjgiIHI9IjEuNCIgZmlsbD0iI2FlYmJjNSIvPjxwYXRoIGQ9Im00IDE3IDUtNSA0IDQgMy0zIDQgNCIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjYWViYmM1IiBzdHJva2Utd2lkdGg9IjEuNCIvPjxwYXRoIGQ9Im0zIDMgMTggMTgiIHN0cm9rZT0iI2MzY2RkNCIgc3Ryb2tlLXdpZHRoPSIyIi8+PC9nPjwvc3ZnPg==";
                element.classList.add("chat-picture-img-default", "chat-picture-deleted");
                element.classList.remove("local-client-chat-picture-img", "imgnotsentyet");
                element.alt = "Image deleted";
                element.title = "Image deleted";
            }
            else
            {
                let file_card = document.querySelector('.chat-file-card[data-single-chat-message-server-message-id="' + msg.message.chat_message_id + '"]');

                if (file_card != null)
                {
                    chat_reactions__forget(file_card, true);
                    chat_files__mark_chat_file_card_deleted(file_card);
                }
                else
                {
                    console.log("process_chat_message_delete_from_server unable to find element");
                }
            }
        }
    },
    /**
     * @brief a poke: shows its text in an alert with the poke sound, unless the poker is ignored
     *
     * @param object msg -> the server message, msg.message holds client_id and poke_message
     *
     * @return void
     */
    process_poke_from_server: function(msg)
    {
        // the sender may be missing from the client list (hidden password channel, or a poke that
        // arrives before the list is built); a null entry must not end the handler
        let poke_sender = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (poke_sender != null && poke_sender.is_ignored_by_local_client == true)
        {
            return;
        }

        if (g_are_sound_effects_enabled)
        {
            g_sound_effects.poke.play();
        }
        let poke_sender_name = channel_tree__get_display_name_by_client_id(msg.message.client_id, "");
        let string1 = "" + chat__sanitize_string(poke_sender_name) + " says : " + chat__sanitize_string(msg.message.poke_message);
        utils__custom_alert(string1);

        // in the wrapper app the alert above is drawn inside a webview nobody is looking
        // at while the app is in the background - which is when a poke actually matters.
        // hand it to android too, it decides whether a notification is needed
        if (g_is_running_in_android_webview == true && typeof Android !== "undefined")
        {
            try { Android.JavaExportShowPokeNotification(poke_sender_name, msg.message.poke_message); }
            catch (e) { console.warn("poke notification bridge failed: " + e.message); }
        }
    },
    /**
     * @brief Replaces an authorized edit and marks the message as edited.
     * @param msg Message identifier, changed text and requester information.
     */
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
            element.textContent = msg.message.new_message_value;
            element.classList.add("chat-message-edited");
        }
    },
    /**
     * @brief Stores an uploaded icon and continues any remaining uploads.
     *
     * @param object msg -> the server message, msg.message holds icon_id and base64_icon
     *
     * @return void
     */
    process_icon_add_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let icon = {
            id: msg.message.icon_id,
            base64_icon: msg.message.base64_icon
        }

        g_icons.push(icon);


        // batch upload: if this reply is the icon we just sent, send the next queued one
        if (g_icon_upload_in_flight_base64 != null && msg.message.base64_icon == g_icon_upload_in_flight_base64)
        {
            g_icon_upload_in_flight_base64 = null;
            server_settings_tab__send_next_queued_icon_upload();
            if (typeof groups__icon_uploaded === "function")
            {
                groups__icon_uploaded();
            }
        }
    },
    /**
     * @brief a group was taken off a client: drops the id from their group_ids and removes its label from their row
     *
     * @param object msg -> the server message, msg.message holds client_id and group_id
     *
     * @return void
     */
    process_remove_group_from_client_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        console.log(msg);
        let client = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (client == null)
        {
            return;
        }

        // do not add duplicate group ids to client object

        if (client.group_ids.includes(msg.message.group_id))
        {
            client.group_ids.splice(client.group_ids.indexOf(msg.message.group_id), 1);  // deleting
        }

        // double selector..first find client groups element
        let client_groups = document.getElementById("client-groups-"+msg.message.client_id);
        let target_element = client_groups ? client_groups.querySelector('.single-group[group-id="'+msg.message.group_id+'"]') : null;
        if (target_element != null)
        {
            target_element.remove();
        }
        else
        {
            console.log("process_remove_group_from_client_from_server failed to find group-id:" + msg.message.group_id);
        }
    },
    /**
     * @brief Updates saved contacts using their identity hashes, names and permitted public keys.
     * @param msg Saved contact list from the server.
     */
    process_stored_clients_list_from_server: function(msg)
    {
        g_offline_client_list = [];
        if (G_HAS_DOM && (!g_server_policy.is_stored_client_list_allowed
            || !groups__is_allowed(channel_tree__get_client_by_client_id(g_local_client_id), "view_stored_users")))
        {
            UI.schedule_member_list_sync();
            return;
        }

        if (msg.message.stored_clients == null)
        {
            return;
        }

        for (let i = 0; i < msg.message.stored_clients.length; i++)
        {
            let entry = msg.message.stored_clients[i];

            if (entry == null || typeof entry.identity_hash !== "string" || !/^[A-Za-z0-9+/=]+$/.test(entry.identity_hash))
            {
                continue; // nothing to name or pair it by
            }

            g_offline_client_list.push({
                identity_hash: entry.identity_hash,
                username: typeof entry.username === "string" ? entry.username : "Saved user",
                base64_avatar: (typeof entry.base64_avatar === "string") ? entry.base64_avatar : "",
                group_ids: (entry.group_ids != null) ? entry.group_ids : [],
                // unix seconds; only sent when the server has last-seen enabled
                last_seen: (typeof entry.last_seen === "number") ? entry.last_seen : 0,
                // The server includes the key only when both users have the required offline messaging permissions.
                public_key: (typeof entry.public_key === "string") ? entry.public_key : ""
            });
        }

        utils__custom_log("stored clients list: " + g_offline_client_list.length + " people kept of " + msg.message.stored_clients.length + " sent");
        UI.schedule_member_list_sync();
    },


    /**
     * @brief the country code a client's row shows from now on; the server withholds an admin's, so it arrives as "" when they become admin and comes back when they stop being one
     *
     * @param object msg -> the server message, msg.message holds client_id and country_iso_code
     *
     * @return void
     */
    process_client_country_code_changed_from_server: function(msg)
    {
        let client_object = channel_tree__get_client_by_client_id(msg.message.client_id);
        if (client_object == null)
        {
            return;
        }

        client_object.country_iso_code = (msg.message.country_iso_code != null) ? msg.message.country_iso_code : "";

        // repaint the flag on the live rows in place; the member list mirror re-clones them right after
        let rows = document.querySelectorAll('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]');
        for (let i = 0; i < rows.length; i++)
        {
            let flag = rows[i].querySelector(".connected-client-country-flag");
            if (flag != null)
            {
                flag.className = "connected-client-country-flag country-flag-" + client_object.country_iso_code.toLowerCase();
            }
        }

        UI.schedule_member_list_sync();
    },

    /**
     * @brief a group was given to a client: recorded on the client (no duplicates) and painted as a label; also reveals the server-settings button when the local client just received the admin group
     *
     * @param object msg -> the server message, msg.message holds client_id and group_id
     *
     * @return void
     */
    process_add_group_to_client_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let client = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (client == null)
        {
            return;
        }

        // do not add duplicate group ids to client object

        let group = channel_tree__get_group_by_group_id(msg.message.group_id);

        // resolve the group BEFORE recording it - an unknown id used to be pushed onto the
        // client and only then bailed on, leaving an id no later render could resolve
        if (group == null)
        {
            console.log("group is null");
            return;
        }

        if (!client.group_ids.includes(msg.message.group_id))
        {
            client.group_ids.push(msg.message.group_id);
        }

        let target_element = document.getElementById("client-groups-" + msg.message.client_id);

        // a client in a hidden channel has no row to paint on. this used to throw here and
        // skip everything below it, including revealing the local admin's settings button
        if (target_element != null)
        {
            const node = document.createElement("div");
            node.className = "single-group";
            node.setAttribute("group-id", group.group_id);

            let icon = group.has_icon ? channel_tree__get_icon_by_icon_id(group.group_linked_icon_id) : null;

            if (icon != null)
            {
                node.style.backgroundImage = "url("+icon.base64_icon+")";
            }

            target_element.appendChild(node);
        }


    },
    /**
     * @brief Adds a group received from the server.
     *
     * @param object msg -> the server message, msg.message holds the group's fields
     *
     * @return void
     */
    process_group_add_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        g_groups.push(msg.message);
    },
    /**
     * @brief Removes a deleted group from the list and from connected users.
     *
     * @param object msg -> the server message, msg.message.group_id names the group
     *
     * @return void
     */
    process_group_delete_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let group_id = msg.message.group_id;

        for (let i = g_groups.length - 1; i >= 0; i--)
        {
            if (g_groups[i].group_id == group_id) { g_groups.splice(i, 1); }
        }

        let displayed_groups = document.querySelectorAll('.single-group[group-id="' + group_id + '"]');
        for (let i = 0; i < displayed_groups.length; i++) { displayed_groups[i].remove(); }

        for (let i = 0; i < g_client_list.length; i++)
        {
            let index_of_group = g_client_list[i].group_ids.indexOf(group_id);
            if (index_of_group != -1) { g_client_list[i].group_ids.splice(index_of_group, 1); }
        }
    },
    /**
     * @brief Removes a deleted server icon and updates the places that displayed it.
     *
     * @param object msg -> the server message, msg.message.icon_id names the icon
     *
     * @return void
     */
    process_icon_delete_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let icon_id = msg.message.icon_id;

        for (let i = g_icons.length - 1; i >= 0; i--)
        {
            if (g_icons[i].id == icon_id)
            {
                g_icons.splice(i, 1);
            }
        }

        UI.refresh_all_channel_icons();
        UI.schedule_member_list_sync();
    },
    /**
     * @brief a channel's icon changed: the new icon fields are stored and its row repainted (and the icon box of the channel edit form if it is open for that channel)
     *
     * @param object msg -> the server message, msg.message holds channel_id, has_channel_icon and channel_icon_id
     *
     * @return void
     */
    process_channel_icon_changed_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let channel = channel_tree__get_channel_by_id(g_channel_list, msg.message.channel_id);
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
    /**
     * @brief Updates the icon shown beside users with this group.
     *
     * @param object msg -> the server message, msg.message holds group_id, has_icon and group_linked_icon_id
     *
     * @return void
     */
    process_group_icon_changed_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let group_id = msg.message.group_id;
        let has_icon = msg.message.has_icon;
        let icon_id = msg.message.group_linked_icon_id;

        for (let i = 0; i < g_groups.length; i++)
        {
            if (g_groups[i].group_id == group_id)
            {
                g_groups[i].has_icon = has_icon;
                g_groups[i].group_linked_icon_id = icon_id;
                break;
            }
        }

        let icon = has_icon ? channel_tree__get_icon_by_icon_id(icon_id) : null;
        let background = (icon != null) ? ("url(" + icon.base64_icon + ")") : "";

        let displayed_groups = document.querySelectorAll('.single-group[group-id="' + group_id + '"]');
        for (let i = 0; i < displayed_groups.length; i++)
        {
            displayed_groups[i].style.backgroundImage = background;
        }
    },
    /**
     * @brief a client started streaming a song: the marquee on their row shows the song name
     *
     * @param object msg -> the server message, msg.message holds client_id and song_name
     *
     * @return void
     */
    process_start_song_stream_from_server: function(msg)
    {
        let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + msg.message.client_id + '"]');
        if (element != null)
        {
            element.style.display = "inline-block";
            document.getElementById("marquee-song-name-client-id-" + msg.message.client_id).innerHTML = chat__sanitize_string(msg.message.song_name);
        }
        else
        {
            console.log("could not find element");
        }
    },

    /**
     * @brief a music bot's song list: opens the musicbot management dialog and rebuilds its song table; each row's X sends a remove-song request
     *
     * @param object msg -> the server message, msg.message.songs holds the songs
     *
     * @return void
     */
    process_music_bot_song_list_from_server: function(msg)
    {
        console.log(msg);

        document.getElementById("musicbot-management-background-container").style.display = "block";

        let html_to_append = "";

        for (single_song of msg.message.songs)
        {
            html_to_append += '\n\
            <tr>\n\
                <td class="musicbot-song-name-cell">'+ single_song.name +'</td>\n\
                <td class="musicbot-song-duration-cell">'+ single_song.duration_seconds +'</td>\n\
                <td class="musicbot-song-remove-cell"><input class="remove-song-from-musicbot-button button-1" type="button" data-song-id="'+single_song.id+'" value="remove"></td>\n\
            </tr>';
        }

        document.getElementById("musicbot-management-background-container-tbody").innerHTML = html_to_append;

        for (let x = 0; x < document.getElementsByClassName("remove-song-from-musicbot-button").length; x++)
        {
            document.getElementsByClassName("remove-song-from-musicbot-button")[x].onclick = function(event) {
                event.stopPropagation();

                let song_id = parseInt(event.currentTarget.getAttribute("data-song-id"));

                client_msg.send_remove_song_from_music_bot_request(song_id, parseInt(g_selected_client_id));
            };
        }
    },

    /**
     * @brief our upload was acknowledged: the lock is released and the server is told what the finished file is for, by sending the current file_send_intent back
     *        one upload = one completion; the intent is cleared so a stray ack cannot replay the last file
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_file_send_success_from_server: function(msg)
    {
        let was_picture = (g_file_send_intent == "direct_chat_picture_file" || g_file_send_intent == "channel_chat_picture_file");

        // the ack means every part actually arrived, so this is where the upload is really
        // finished. released before the intent check so a stray ack cannot leave the lock stuck
        chat__release_file_upload_lock();

        if (g_file_send_intent == "musicbot_file" || g_file_send_intent == "direct_chat_picture_file" || g_file_send_intent == "channel_chat_picture_file"
            || g_file_send_intent == "direct_chat_file" || g_file_send_intent == "channel_chat_file")
        {
            client_msg.send_file_send_completed_request(g_file_send_intent);

            // one upload = one completion. clear the intent so a later stray file_send_success
            // cannot replay the last file (e.g. re-upload the song on an unrelated action)
            g_file_send_intent = "";
            g_file_send_intent_extra_data = {};
        }

        // the picture is on the server now; image_sent_status marks the end of the relay
        if (was_picture == true)
        {
            chat__show_picture_delivery_status();
        }
    },

    /**
     * @brief one base64 chunk of an incoming file, accumulated in received_files (keyed by server_chat_message_id) with a refreshed last-received timestamp
     *
     * @param object msg -> the server message, msg.message holds server_chat_message_id and the chunk
     *
     * @return void
     */
    process_file_receive_chunk_from_server_from_server: function(msg)
    {
        let is_found = false;
        for (i = 0; i < received_files.length; i++)
        {
            if(received_files[i].file_id == msg.message.server_chat_message_id)
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

            received_files.push(single_file);
        }

        for (i = 0; i < received_files.length; i++)
        {
            if(received_files[i].file_id == msg.message.server_chat_message_id)
            {
                received_files[i].timestamp_last_received = new Date().valueOf();
                received_files[i].file_content_base64 += msg.message.value;

                // a file card's ring; a no-op for pictures, which register no transfer
                chat_files__update_chat_file_progress(msg.message.server_chat_message_id, received_files[i].file_content_base64.length);
                break;
            }
        }
    },

    /**
     * @brief an incoming file is complete: the base64 goes to the data-processing worker for decryption (direct or channel chat picture) and leaves received_files
     *
     * @param object msg -> the server message, msg.message.server_chat_message_id names the file
     *
     * @return void
     */
    process_file_receive_completed_from_server_from_server: function(msg)
    {
        let message_raw = "";
        let index_to_delete = 0;
        let is_found = false;
        for (i = 0; i < received_files.length; i++)
        {
            if(received_files[i].file_id == msg.message.server_chat_message_id)
            {
                is_found = true;
                index_to_delete = i;
                message_raw = received_files[i].file_content_base64;
                break;
            }
        }

        if (is_found == false)
        {
            return;
        }

        // remove before passing it on: this is the only removal from received_files, and a leftover entry
        // would have the next picture with this message id appended on top of the leftovers
        received_files.splice(index_to_delete, 1);

        if (msg.message.receive_type == "direct_chat_picture")
        {
            // loopback: no private key here, node sends the decrypted picture instead
            if (android_host__is_ui_only_runtime())
            {
                return;
            }

            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_direct_chat_picture_data",
                message_raw: message_raw,
                picture_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });

            console.log("total files in received_files " + received_files.length +"");
        }
        else if (msg.message.receive_type == "channel_chat_picture")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_channel_chat_picture_data",
                message_raw: message_raw,
                picture_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });

            console.log("total files in received_files " + received_files.length +"");
        }
        else if (msg.message.receive_type == "direct_chat_file")
        {
            // loopback: no private key here, node sends the decrypted file instead
            if (android_host__is_ui_only_runtime())
            {
                return;
            }

            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_direct_chat_file_data",
                message_raw: message_raw,
                file_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });
        }
        else if (msg.message.receive_type == "channel_chat_file")
        {
            g_data_processing_worker.postMessage({
                type: "mainthread__process_encrypted_channel_chat_file_data",
                message_raw: message_raw,
                file_id: msg.message.server_chat_message_id,
                sender_id: msg.message.sender_id
            });
        }
    }
};

var client_msg = {
    /**
     * @brief asks the server to move the local client into idle mode
     *
     * @return void
     */
    send_go_to_idle_mode_request: function()
    {
        let message_object = {
            message:
            {
                type: "go_to_idle_mode_request",
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief Requests stored users only when the server enables the list and this client may view it.
     *
     * @return void
     */
    send_request_stored_clients: function()
    {
        let client = channel_tree__get_client_by_client_id(g_local_client_id);
        if (!g_is_client_list_retrieved || g_fast_reconnect.resumed
            || !g_server_policy.is_stored_client_list_allowed || !groups__is_allowed(client, "view_stored_users"))
        {
            return;
        }

        let message_object = {
            message:
            {
                type: "request_stored_clients",
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief asks the server to bring the local client back from idle into a channel
     *
     * @param number|null channelId -> the channel, null for root
     *
     * @return void
     */
    send_come_from_idle_mode_request: function(channelId = null)
    {
        let message_object = {
            message:
            {
                type: "come_back_from_idle_mode_request",
                channel_id: channelId,
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief asks the server to rename a client
     *
     * @param string username_to_set -> the new name
     * @param number client_id -> the client
     *
     * @return void
     */
    send_change_client_username_request: function(username_to_set, client_id)
    {
        let message_object = {
            message: {
                type: "change_client_username",
                client_id: client_id,
                new_username: username_to_set
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief requests the song list of the selected music bot (g_selected_client_id)
     *
     * @return void
     */
    send_musicbot_song_list_request: function()
    {
        let message_object = {
            message:
            {
                type: "musicbot_get_song_list",
                musicbot_id: parseInt(g_selected_client_id),
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief uploads one base64 part of a file; index 0 flags the start of a new file
     *        the server merges the parts and, once done, asks what the file is for
     *
     * @param number total_bytes_length -> the whole file's length
     * @param string data_part_base64 -> this part
     * @param number index -> the part's index
     *
     * @return void
     */
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

        connection__send_message_object(message_object);
    },
    /**
     * @brief tells the server what the fully uploaded file is for: the intent plus the global g_file_send_intent_extra_data
     *
     * @param string g_file_send_intent -> the intent, "musicbot_file", "channel_chat_file" and the like
     *
     * @return void
     */
    send_file_send_completed_request: function(g_file_send_intent)
    {
        // after file upload is done, server informs the client that it is done. Clients then tells the server what to do with the file (file_send_intent)

        let message_object = {
            message: {
                type: "file_send_completed",
                file_send_intent: g_file_send_intent,
                file_send_intent_extra_data: g_file_send_intent_extra_data
            }
        };

        connection__send_message_object(message_object);
    },

    /**
     * @brief asks a music bot to delete one of its songs
     *
     * @param number song_id -> the song
     * @param number musicbot_client_id -> the bot
     *
     * @return void
     */
    send_remove_song_from_music_bot_request: function(song_id, musicbot_client_id) {
        let message_object = {
                message: {
                    type: "remove_song_from_music_bot",
                    musicbot_id: musicbot_client_id,
                    song_id: song_id
                }
            };

        connection__send_message_object(message_object);
    }
};
