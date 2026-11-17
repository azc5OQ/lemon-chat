// handles server messages and builds client requests for the browser and Node
// dispatch.js routes worker results to these handlers

var is_long_press = null;

/**
 * @brief creates the stored client record from a server list entry
 *
 * @param object client -> the server client record
 * @param string username -> the sanitized username
 * @param boolean is_channel_hidden -> the channel visibility flag to store
 *
 * @return object -> the client record with local state reset
 */
function messages__create_client_record(client, username, is_channel_hidden)
{
    let single_client =
    {
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

    single_client.client_id = parseInt(client.client_id);
    single_client.username = username;
    single_client.identity_hash = (client.identity_hash != null) ? client.identity_hash : "";
    single_client.public_key = client.public_key;
    single_client.channel_id = client.channel_id;
    single_client.channel_group_id = Number(client.channel_group_id) || 0;
    single_client.audio_state = client.audio_state;
    single_client.group_ids = client.group_ids;
    single_client.is_clients_channel_hidden = is_channel_hidden;
    single_client.country_iso_code = client.country_iso_code;
    single_client.is_idle = client.is_idle;
    single_client.is_ignored_by_local_client = false;
    single_client.is_muted_by_local_client = false;
    single_client.unread_count = 0;
    single_client.is_music_bot = client.is_music_bot;
    single_client.music_bot_encryption = client.music_bot_encryption || 0;
    single_client.music_bot_keys = client.music_bot_keys || null;
    return single_client;
}

/**
 * @brief refreshes open channel tab names from the server channel list
 *
 * @return void
 */
function messages__refresh_channel_context_names()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    for (let tab of document.querySelectorAll('.chat-context-selector[data-chat-context-selector-type="channel"]'))
    {
        let id = tab.getAttribute("data-chat-context-selector-id");
        let match = /^channel-(\d+)$/.exec(id);
        let channel = match ? channel_tree__get_channel_by_id(g_channel_list, Number(match[1])) : null;
        let title = tab.querySelector(".p-container > p");
        if (channel != null && title != null && title.textContent !== channel.name)
        {
            title.textContent = channel.name;
        }
    }
}

var server_msg = {
    /**
     * @brief applies the permission values announced by the server
     *
     * @param msg -> changed group and its permissions
     */
    process_group_permissions_changed: function(msg)
    {
        let group = channel_tree__get_group_by_group_id(msg.message.group_id);
        if (group)
        {
            group.permissions = msg.message.permissions;
        }
        if (typeof groups__permissions_saved === "function")
        {
            groups__permissions_saved(msg.message.group_id, msg.message.permissions);
        }
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
    },
    /**
     * @brief updates the group list and the groups beside connected users
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
        channel_groups.catalog = Array.isArray(msg.message.channel_groups) ? msg.message.channel_groups : [];
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

                // hidden clients have no channel row to update
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
     * @brief updates the saved users shown beside the selected server group
     *
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
     * @brief replaces the server icon list and refreshes icons shown in channels and groups
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

        // refresh rows created before the icon list arrived
        UI.refresh_all_channel_icons();

        // group pictures too
        if (typeof channel_groups__refresh_icons === "function")
        {
            channel_groups__refresh_icons();
        }
    },
    /**
     * @brief builds the channel tree and its controls from the initial server list
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
                // add each unseen channel to the local list and tree

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

                // complete the parent after its last child has been processed

                let is_channel_already_processed = processed_channels.indexOf(child_channels[a].channel_id) != -1;

                if (is_channel_already_processed == true)
                {
                    if ((a + 1) == child_channels.length)
                    {
                        if (current_parent_channel_id_to_find == g_ROOT_LEVEL_PARENT_MARKER)
                        {
                            continue;
                        }

                        // mark this parent complete after all its children

                        processed_channels.push(current_parent_channel_id_to_find);
                    }
                }
                else
                {
                    // descend into children or mark a leaf complete

                    var children_of_child_channel = channel_tree__get_channels_by_channel_parent_id(msg.message.channels, child_channels[a].channel_id);
                    if (children_of_child_channel.length > 0)
                    {
                        current_parent_channel_id_to_find = child_channels[a].channel_id;
                        break;
                    }

                    processed_channels.push(child_channels[a].channel_id);
                }
            }

            // stop after repeated passes without progress so invalid channel trees cannot freeze the page
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

        messages__refresh_channel_context_names();

        // recompute chevron (leaf) visibility after the channel tree changed
        UI.refresh_all_channel_fullness();
    },
    /**
     * @brief clears the client's song indicator and stops local streaming when applicable
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_stop_song_stream_from_server: function(msg)
    {
        const client_index = channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id);
        const boundary = msg.message.discard_audio_through_counter;
        if (client_index >= 0 && g_client_list[client_index].is_music_bot && typeof boundary === "string" && /^[0-9]{1,20}$/.test(boundary))
        {
            const discard_through = BigInt(boundary);
            if (discard_through <= 18446744073709551615n)
            {
                const previous = g_client_list[client_index].discard_audio_through_counter;
                if (previous == null || discard_through >= previous)
                {
                    g_client_list[client_index].discard_audio_through_counter = discard_through;
                    g_opus_decoder_worker.postMessage({
                        type: "discard_sender_audio",
                        sender_id: msg.message.client_id,
                        counter: boundary
                    });
                }
            }
        }
        // song stream messages control the title marquee. microphone state is handled separately

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

        // stop local song uploads when the server ends this client's stream

        if (msg.message.client_id == g_local_client_id)
        {
            g_stop_song_stream_message_received = true;
        }
    },
    /**
     * @brief applies a channel video stream state change
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
     * @brief offers the channel video stream to the local client
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
     * @brief handles a request to view the local video stream
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
     * @brief updates a viewer's connection state
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
     * @brief requests a fresh video keyframe for a viewer
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
     * @brief reports a refused video stream request
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
     * @brief adds a connected client, restores their chat and refreshes channel keys
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
                sounds__play("user_entered_your_channel");
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
        single_client.channel_group_id = Number(msg.message.channel_group_id) || 0;
        single_client.group_ids = (msg.message.group_ids != null) ? msg.message.group_ids : [];
        single_client.audio_state = msg.message.audio_state;

        // a channel ID of -1 hides the client's location
        single_client.is_clients_channel_hidden = (msg.message.channel_id == -1);
        single_client.country_iso_code = msg.message.country_iso_code;
        single_client.is_idle = false;
        single_client.is_ignored_by_local_client = false;
        single_client.is_muted_by_local_client = false;
        single_client.unread_count = 0;
        single_client.is_music_bot = msg.message.is_music_bot;
        single_client.music_bot_encryption = msg.message.music_bot_encryption || 0;
        single_client.music_bot_keys = msg.message.music_bot_keys || null;

        chat__remember_peer_state();
        channel_tree__forget_client(single_client.client_id);
        g_client_list.push(single_client);
        g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
        chat__restore_peer_state();
        keys__channel_member_joined(single_client);

        // keep offline conversation history when its owner connects
        let promotion = chat__promote_offline_chat_context_state(single_client);
        UI.promote_offline_chat_context_render(promotion, single_client);

        // a client in a channel this client may not look into is known but has no row in the channel list
        if (single_client.is_clients_channel_hidden == true)
        {
            return;
        }

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

                    // capture the target before the event ends and currentTarget becomes null

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

                    clearTimeout(local_touch_press_timer); // cancel the pending long press when the touch ends

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

        // rebind focusout after replacing the row markup
        {
            let rename_input = document.getElementById('connected-local-client-input');
            if (rename_input != null)
            {
                rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout);
            }
        }

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

            // check has_icon because zero is a valid icon ID
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

        // register the bot before audio arrives. Node has no audio player
        if (typeof audio__audio_player_announce_music_bots === "function")
        {
            audio__audio_player_announce_music_bots();
        }
    },
    /**
     * @brief removes a disconnected client and refreshes the saved contact list
     *
     * @param msg server message naming the disconnected client
     */
    process_client_disconnect_from_server: function(msg)
    {
        if (typeof groups__schedule_refresh === "function")
        {
            groups__schedule_refresh();
        }
        let disconnecting_client = channel_tree__get_client_by_client_id(msg.message.client_id);
        let local_client = channel_tree__get_client_by_client_id(g_local_client_id);

        // remove the user before updating chat panels or playing a sound
        // a missing panel must not leave a disconnected user in the channel list
        chat__remember_peer_state();
        channel_tree__forget_client(msg.message.client_id);
        keys__channel_member_left(disconnecting_client);
        chat__clear_typing_from_client(msg.message.client_id);

        // the client may disconnect before its local record is created
        if (disconnecting_client != null && local_client != null && disconnecting_client.channel_id == local_client.channel_id)
        {
            if (g_are_sound_effects_enabled)
            {
                sounds__play(msg.message.reason === "ban" ? "user_in_your_channel_was_banned_from_the_server" : "user_disconnected_from_your_channel");
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
     * @brief notifies Android of an incoming call through the active native bridge
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

        if (android_host__is_ui_only_runtime())
        {
            return; // Node owns the correlated native call record
        }

        if (g_node_incoming_call_listener != null)
        {
            android_host__incoming_call(client.username, client.channel_id);
            return;
        }

        utils__custom_alert("incoming call from " + client.username);
    },
    /**
     * @brief restores a client from idle and updates its channel before processing channel keys
     *
     * @param object msg -> the server message, msg.message holds client_id and channel_id
     *
     * @return void
     */
    process_client_coming_back_from_idle_mode_from_server: function(msg)
    {
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].channel_id = msg.message.channel_id;
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].channel_group_id = Number(msg.message.channel_group_id) || 0;
        groups__schedule_refresh();
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = false;

        // update the local channel before accepting its maintainer and keys
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

        let single_client = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];
        single_client.is_clients_channel_hidden = (msg.message.channel_id == -1);
        if (!G_HAS_DOM)
        {
            return;
        }

        if (document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]') != null)
        {
            document.querySelector('.connected-client[data-connected-client-id="' + msg.message.client_id + '"]').remove();
        }

        // do not add a channel row when this client's location is hidden
        if (single_client.is_clients_channel_hidden == true)
        {
            return;
        }

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

                // capture the target before the event ends and currentTarget becomes null

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

                clearTimeout(local_touch_press_timer); // cancel the pending long press when the touch ends

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
                sounds__play("user_joined_your_channel");
                sounds__stop("call");
            }
        }

        // continuous transmission starts only when toggled by the user

    },
    /**
     * @brief applies the server's data channel offer and sends the local answer
     *
     * @param object msg -> the server message, msg.message.value is the offer as json
     *
     * @return void
     */
    process_sdp_offer_from_server: function(msg)
    {
        // Node forwards this offer but has no WebRTC peer connection
        if (g_peer_connection_with_server == null)
        {
            return;
        }


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
     * @brief removes a deleted channel and reports local deletion success
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
                sounds__play("channel_deleted");
            }
        }

        let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        if (channel_index != -1)
        {
            g_channel_list.splice(channel_index, 1);
            if (G_HAS_DOM)
            {
                document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').remove();
                UI.refresh_all_channel_fullness();
            }
        }
    },
    /**
     * @brief adds a server ICE candidate to the current peer connection
     *
     * @param object msg -> the server message, msg.message.value is the candidate
     *
     * @return void
     */
    process_ice_candidate_from_server: function(msg)
    {
        // Node has no peer connection. a browser candidate may also arrive after teardown
        if (g_peer_connection_with_server == null)
        {
            return;
        }

        console.log("got ice candidate from server");
        g_peer_connection_with_server.addIceCandidate(msg.message.value);
    },
    /**
     * @brief appends a server broadcast to the root conversation
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
     * @brief records the delivered message identifier and enables its available actions
     *
     * @param object msg -> the server message, msg.message holds local_message_id and server_chat_message_id
     *
     * @return void
     */
    process_server_chat_message_id_for_local_message_id_from_server: function(msg)
    {
        // a late acknowledgement must not keep author data for a message already removed locally
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
     * @brief updates a channel's properties and reports local edit success
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
                sounds__play("channel_edited");
            }
        }

        let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        g_channel_list[index].description = msg.message.channel_description;
        g_channel_list[index].is_description_hidden = (msg.message.is_description_hidden === true);
        g_channel_list[index].is_using_password = msg.message.is_using_password;
        g_channel_list[index].name = msg.message.channel_name;
        messages__refresh_channel_context_names();
        g_channel_list[index].is_audio_enabled = msg.message.is_audio_enabled;

        if (typeof msg.message.is_temp_channel === "boolean")
        {
            g_channel_list[index].is_temp_channel = msg.message.is_temp_channel;
            moderation.channels.delete(Number(msg.message.channel_id));
        }
        g_channel_list[index].is_video_stream_enabled = (msg.message.is_video_stream_enabled != false);
        g_channel_list[index].is_client_limit_active = msg.message.is_client_limit_active;
        g_channel_list[index].max_client_count = msg.message.max_client_count;
        g_channel_list[index].required_talk_power = Number(msg.message.required_talk_power) || 0;
        g_channel_list[index].required_message_power = Number(msg.message.required_message_power) || 0;
        g_channel_list[index].required_picture_power = Number(msg.message.required_picture_power) || 0;
        g_channel_list[index].required_stream_power = Number(msg.message.required_stream_power) || 0;
        g_channel_list[index].required_file_power = Number(msg.message.required_file_power) || 0;
        g_channel_list[index].required_view_power = Number(msg.message.required_view_power) || 0;
        g_channel_list[index].required_create_power = Number(msg.message.required_create_power) || 0;
        if (!G_HAS_DOM)
        {
            return;
        }

        document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2].innerHTML = chat__sanitize_string(msg.message.channel_name);

        // a theme with the conversation list shows or hides its talk bar by this setting
        if (typeof member_conversations__schedule_refresh === "function")
        {
            member_conversations__schedule_refresh();
        }
        if (typeof msg.message.is_temp_channel === "boolean")
        {
            document.querySelector('[data-channel-id="' + msg.message.channel_id + '"]').children[2] .classList.toggle("single-channel-is-temp", msg.message.is_temp_channel);
            if (moderation.channel_edit === Number(msg.message.channel_id))
            {
                let type = document.getElementById("channel-properties-type");
                type.value = msg.message.is_temp_channel ? "temporary" : "permanent";
                type.dataset.originalValue = type.value;
                UI.refresh_channel_type_options(Number(msg.message.channel_id));
                moderation__send("channel_access_request", { channel_id: Number(msg.message.channel_id) });
            }
        }
        video__refresh_controls();
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
     * @brief applies the client details that the server permitted this user to request
     *
     * @param object msg -> the server message, msg.message holds the info fields
     *
     * @return void
     */
    process_client_info_from_server: function(msg)
    {
        if (G_HAS_DOM)
        {
            client_status__receive(msg.message);
        }
    },

    /**
     * @brief adds a channel below its parent and attaches its controls
     *
     * @param object msg -> the server message, msg.message holds the channel's fields and channel_creator_id
     *
     * @return void
     */
    process_channel_create_from_server: function(msg)
    {


        if (msg.message.channel_creator_id == g_local_client_id)
        {
            if (g_are_sound_effects_enabled)
            {
                sounds__play("channel_created");
            }
        }

        let new_channel = {
            channel_id: msg.message.channel_id,
            parent_channel_id: msg.message.parent_channel_id,
            name: msg.message.name,
            description: msg.message.description,
            is_description_hidden: (msg.message.is_description_hidden === true),
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
            required_file_power: Number(msg.message.required_file_power) || 0,
            required_view_power: Number(msg.message.required_view_power) || 0,
            required_create_power: Number(msg.message.required_create_power) || 0,
            has_channel_icon: msg.message.has_channel_icon,
            channel_icon_id: msg.message.channel_icon_id,
            is_channel_directly_collapsed: false,
        };

        g_channel_list.push(new_channel);
        if (!G_HAS_DOM)
        {
            return;
        }
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

        // append after the parent's clients and existing subtree to preserve sibling order
        let subtree_anchor = channel_tree__get_channel_subtree_last_element(new_channel.parent_channel_id);
        subtree_anchor.insertAdjacentHTML("afterend", html_to_append);

        // apply any icon included with the new channel
        UI.refresh_channel_icon(new_channel);

        let elements = document.getElementsByClassName('single-channel');

        // handle UI differently on touch devices
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
     * @brief creates a receiving picture placeholder for an accepted channel message
     *
     * @param object msg -> the server message, msg.message holds sender_id, picture_id and size
     *
     * @return boolean|string true when stored, false to retry, or "discarded" for rejected content
     */
    process_channel_chat_picture_metadata_from_server: function(msg)
    {
        const sender = chat__resolve_direct_sender(msg);
        if (sender == null)
        {
            return false;
        }
        if (sender.is_ignored_by_local_client)
        {
            return "discarded";
        }

        // the ring counts the incoming chunks against the encrypted length the server announced
        if (!chat_files__begin_chat_file_transfer(msg.message.picture_id, msg.message.size))
        {
            return "discarded";
        }

        let sender_username = sender.username;
        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let index = chat__get_chat_context_index_by_chat_context_id(receiving_chat_context_id);
        if (index < 0 || document.getElementById(receiving_chat_context_id) == null)
        {
            return false;
        }
        g_chat_message_author_public_keys[msg.message.picture_id] = sender.public_key;

        g_chat_context_array[index].last_known_message_sender_username = sender_username;

        let elements_count = document.getElementsByClassName('chat-spacer').length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName('chat-spacer')[0].remove();
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(sender.client_id, chat__sanitize_string(sender_username)) + "\n\
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
        return !G_HAS_DOM || document.getElementById("chat-picture-img-" + msg.message.picture_id) != null;
    },
    /**
     * @brief adds an accepted private picture placeholder to its sender's conversation
     *
     * @param object msg -> the server message, msg.message holds sender_id, picture_id and size
     *
     * @return boolean|string true when stored, false to retry, or "discarded" for rejected content
     */
    process_direct_chat_picture_metadata_from_server: function(msg)
    {
        const sender = chat__resolve_direct_sender(msg);
        if (sender == null)
        {
            return false;
        }
        if (sender.is_ignored_by_local_client)
        {
            return "discarded";
        }

        // the ring counts the incoming chunks against the encrypted length the server announced
        if (!chat_files__begin_chat_file_transfer(msg.message.picture_id, msg.message.size))
        {
            return "discarded";
        }

        const received_direct_message_chat_context_id = sender.context_id;
        if (!chat__ensure_direct_context(sender))
        {
            return false;
        }

        if (g_are_sound_effects_enabled)
        {
            sounds__play("message_received");
        }

        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);
        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(sender.client_id, chat__sanitize_string(sender.username)) + "\n\
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

        g_chat_message_author_public_keys[msg.message.picture_id] = sender.public_key;
        chat__record_direct_activity(sender);
        g_chat_context_array[chat_context_index].last_known_message_sender_username = sender.username;
        return !G_HAS_DOM || document.getElementById("chat-picture-img-" + msg.message.picture_id) != null;
    },
    /**
     * @brief creates a file card from accepted channel file metadata
     *
     * @param object msg -> the server message, msg.message holds sender_id, channel_id, file_id, the header and size
     *
     * @return boolean|string true when stored, false to retry, or "discarded" for rejected content
     */
    process_channel_chat_file_metadata_from_server: function(msg)
    {
        const sender = chat__resolve_direct_sender(msg);
        if (sender == null)
        {
            return false;
        }
        if (sender.is_ignored_by_local_client)
        {
            return "discarded";
        }

        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(receiving_chat_context_id);

        if (chat_context_index == -1 || document.getElementById(receiving_chat_context_id) == null)
        {
            return false;
        }

        let sender_username = sender.username;
        let header = (msg.message.file_header_decrypted != null) ? msg.message.file_header_decrypted : { name: "(file, no key to open it)", size: 0 };

        g_chat_context_array[chat_context_index].last_known_message_sender_username = sender_username;
        g_chat_message_author_public_keys[msg.message.file_id] = sender.public_key;
        if (!chat_files__begin_chat_file_transfer(msg.message.file_id, msg.message.encrypted_size))
        {
            return "discarded";
        }

        let elements_count = document.getElementsByClassName("chat-spacer").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-spacer")[0].remove();
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(sender.client_id, chat__sanitize_string(sender_username)) + "\n\
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
        return !G_HAS_DOM || card != null;
    },
    /**
     * @brief creates a private file card and updates the sender's unread state
     *
     * @param object msg -> the server message, msg.message holds sender_id, file_id, the header and size
     *
     * @return boolean|string true when stored, false to retry, or "discarded" for rejected content
     */
    process_direct_chat_file_metadata_from_server: function(msg)
    {
        const sender = chat__resolve_direct_sender(msg);
        if (sender == null)
        {
            return false;
        }
        if (sender.is_ignored_by_local_client)
        {
            return "discarded";
        }

        const received_direct_message_chat_context_id = sender.context_id;
        if (!chat__ensure_direct_context(sender))
        {
            return false;
        }

        if (g_are_sound_effects_enabled)
        {
            sounds__play("message_received");
        }

        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);
        let header = (msg.message.file_header_decrypted != null) ? msg.message.file_header_decrypted : { name: "(file, no key to open it)", size: 0 };

        g_chat_message_author_public_keys[msg.message.file_id] = sender.public_key;
        if (!chat_files__begin_chat_file_transfer(msg.message.file_id, msg.message.encrypted_size))
        {
            return "discarded";
        }

        let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(sender.client_id, chat__sanitize_string(sender.username)) + "\n\
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

        chat__record_direct_activity(sender);
        if (g_current_chat_context_id === received_direct_message_chat_context_id)
        {
            chat__scroll_chat_to_end(false);
        }

        g_chat_context_array[chat_context_index].last_known_message_sender_username = sender.username;
        return !G_HAS_DOM || card != null;
    },
    /**
     * @brief validates the channel maintainer and starts key delivery or waiting
     *
     * @param object msg -> the server message, msg.message holds maintainer_id, channel_id and has_maintainer
     *
     * @return void
     */
    process_channel_maintainer_id_from_server: function(msg)
    {
        // a delayed announcement for a channel we left must not change this channel's maintainer
        if (msg.message.channel_id !== g_current_channel_id || !Number.isSafeInteger(msg.message.maintainer_generation) || msg.message.maintainer_generation < 0)
        {
            return;
        }

        // accept a maintainer only if the local client list places them in this channel

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

        // reject a maintainer missing from the local client list
        if (index == -1)
        {
            console.warn("maintainer message for unknown channel " + g_current_channel_id + ", ignoring");
            return;
        }

        if (g_channel_list[index].maintainer_id !== msg.message.maintainer_id || g_channel_list[index].maintainer_generation !== msg.message.maintainer_generation)
        {
            keys__cancel_channel_key_creation();
        }
        g_channel_list[index].maintainer_id = msg.message.maintainer_id;
        g_channel_list[index].maintainer_generation = msg.message.maintainer_generation;
        g_channel_list[index].has_maintainer = msg.message.has_maintainer;
        if (msg.message.has_maintainer && (g_local_client_id == msg.message.maintainer_id) && (g_current_channel_id == msg.message.channel_id))
        {
            // use the validated client index
            console.log("local user is maintainer of channel '" + g_channel_list[index].name + "'");

            // local user is the key SENDER now, there is nothing to wait for
            keys__cancel_maintainer_keys_wait_timer();

            keys__create_and_send_new_channel_keys();
        }
        else if ((g_current_channel_id == msg.message.channel_id) && (g_local_client_id != msg.message.maintainer_id))
        {
            console.log("%c received channel_maintainer_id " + msg.message.maintainer_id + " for channel: " + msg.message.channel_id, "color: blue");

            // vote to replace the maintainer if its keys do not arrive before the timeout
            keys__cancel_channel_key_creation();
            keys__arm_maintainer_keys_wait_timer();
        }
        else
        {
            console.log("%c process_channel_maintainer_id_from_server: unknown", "color: blue");
        }
    },
    /**
     * @brief displays a channel message in its own block with the author and time
     *
     * @param object msg -> the server message, msg.message holds sender_id, server_chat_message_id and the decrypted payload
     *
     * @return boolean|string|undefined false to retry, "discarded" for rejected content, or undefined when handled
     */
    process_channel_chat_message_from_server: function(msg)
    {
        const sender = chat__resolve_direct_sender(msg);
        if (sender == null)
        {
            return false;
        }
        if (sender.is_ignored_by_local_client)
        {
            return "discarded";
        }
        let received_channel_chat_message_object = chat__read_message_payload(msg.message.decrypted_value);
        if (received_channel_chat_message_object == null)
        {
            return "discarded";
        }
        if (chat__receive_message_edit(received_channel_chat_message_object, msg.message, false, sender))
        {
            return;
        }
        if (chat_reactions__receive(received_channel_chat_message_object, msg.message, false, sender))
        {
            return;
        }
        if (sender.client != null)
        {
            chat__clear_typing_from_client(sender.client_id);
        }

        if (sender.public_key === g_rsa_public_key_string)
        {
            return;
        }

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = sender.public_key;

        if (g_are_sound_effects_enabled == true)
        {
            sounds__play("message_received");
        }

        let chat_message_username = chat__sanitize_string(sender.username);
        let receiving_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
        let chat_context_index = chat__get_chat_context_index_by_chat_context_id(receiving_chat_context_id);
        if (chat_context_index < 0 || document.getElementById(receiving_chat_context_id) == null)
        {
            return false;
        }

        // badge the channel unless it is the one on screen
        chat__increment_channel_unread_count(msg.message.channel_id);
        android_host__notify({type: "chat_activity", sender_id: sender.client_id, username: sender.username, context_id: receiving_chat_context_id});

        received_channel_chat_message_object.value = chat__sanitize_string(received_channel_chat_message_object.value);
        let font_size1 = received_channel_chat_message_object.font_size;

        if (!Number.isFinite(font_size1))
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
                                                " + android_host__generate_message_sender_html(sender.client_id, chat_message_username) + "\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-sender-time\">\n\
                                                <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                            </div>\n\
                                            <div class=\"single-chat-message-content\">\n\
                                                <p class='single-chat-message-content-p "+chat__sanitize_font(received_channel_chat_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_color(received_channel_chat_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_channel_chat_message_object.value + "</p>\n\
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
            chat_reactions__register(context.querySelector('[data-single-chat-message-server-message-id="' + msg.message.server_chat_message_id + '"]'), received_channel_chat_message_object.reaction_id, sender.public_key, sender.username, received_channel_chat_message_object.reply_to);
        }
        g_chat_context_array[chat_context_index].last_known_message_sender_username = chat_message_username;
    },
    /**
     * @brief displays a queued message in the conversation belonging to the sender identity
     *
     * @param msg decrypted message and sender details
     *
     * @return string|undefined "discarded" for invalid content, otherwise undefined after handling
     */
    process_offline_chat_message_from_server: function(msg)
    {
        let sender_name = (typeof msg.message.sender_name === "string") ? msg.message.sender_name : "";
        let sender_identity_hash = msg.message.sender_identity_hash;
        if (typeof sender_identity_hash !== "string" || !/^[A-Za-z0-9+/=]+$/.test(sender_identity_hash))
        {
            return;
        }
        let received_object = chat__read_message_payload(msg.message.some_json && msg.message.some_json.value, false);
        if (received_object == null)
        {
            return "discarded";
        }

        let message_text = chat__sanitize_string(received_object.value);
        let when_text = (typeof msg.message.queued_unix_seconds === "number" && msg.message.queued_unix_seconds > 0) ? UI.format_time_ago(msg.message.queued_unix_seconds) : "while you were away";

        let sender_client = null;
        for (let i = 0; i < g_client_list.length; i++)
        {
            if (g_client_list[i] != null && g_client_list[i].identity_hash === sender_identity_hash)
            {
                sender_client = g_client_list[i];
                break;
            }
        }

        const retained_state = chat_peer_local_state.get("identity:" + sender_identity_hash);
        if ((sender_client && sender_client.is_ignored_by_local_client) || (retained_state && retained_state.is_ignored_by_local_client))
        {
            return;
        }
        let context_id = (sender_client != null) ? ("chat-context-pm-" + sender_client.client_id) : ("chat-context-offline-" + sender_identity_hash);
        const sender_contact = sender_client || g_offline_client_list.find(function(client)
        {
            return client.identity_hash === sender_identity_hash;
        });

        // check stored conversations so this also works without rendered tabs
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
                peer_public_key: sender_contact ? sender_contact.public_key : "",
                peer_identity_hash: sender_identity_hash,
                peer_username: sender_name,
                last_known_message_sender_username: ""
            });

            // add a tab so the conversation is reachable outside member list themes
            let selector_id = (sender_client != null) ? ("user-" + sender_client.client_id) : ("offline-" + sender_identity_hash);
            if (document.querySelector('[data-chat-context-selector-id="' + selector_id + '"]') == null)
            {
                let selector_html = "<div class=\"chat-context-selector\" data-chat-context-selector-type=\"user\" data-chat-context-selector-id=\"" + selector_id + "\">\n\
                                        <div class=\"p-container\"><p>" + chat__sanitize_string(sender_name) + "</p></div>\n\
                                        <div class=\"remove-chat-context-selector\" data-chat-context-remove-selector-type=\"user\" data-chat-context-remove-selector-id=\"" + selector_id + "\"></div>\n\
                                    </div>";
                document.getElementById("chat-context-selectors-container").insertAdjacentHTML("beforeend", selector_html);

                // bind handlers after adding the conversation tab
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

        android_host__notify(
        {
            type: "chat_activity",
            sender_id: sender_client ? sender_client.client_id : null,
            username: sender_name,
            context_id: context_id
        });
        // keep unread state under the sender identity even while they are offline
        if (sender_client == null && !android_host__is_ui_only_runtime())
        {
            const state = retained_state || {unread_count: 0, is_ignored_by_local_client: false, is_muted_by_local_client: false};
            state.unread_count++;
            chat_peer_local_state.set("identity:" + sender_identity_hash, state);
            chat__update_app_unread_badge();
        }
        if (sender_client != null)
        {
            chat__increment_unread_count(sender_client.client_id);
            chat__render_unread_badge(sender_client.client_id, true);
        }

        UI.schedule_member_list_sync();
    },

    /**
     * @brief applies decrypted private chat content, controls or channel keys
     *
     * @param object msg -> the server message, msg.message.some_json.type picks the payload
     *
     * @return boolean|string|undefined false to retry, "discarded" for rejected content, or undefined when handled
     */
    process_direct_chat_message_from_server: function(msg)
    {
        if (msg.message.some_json == null || typeof msg.message.some_json !== "object" || Array.isArray(msg.message.some_json) || typeof msg.message.some_json.type !== "string")
        {
            return "discarded";
        }
        const sender = chat__resolve_direct_sender(msg);
        if (sender == null)
        {
            return false;
        }
        if (sender.is_ignored_by_local_client && msg.message.some_json.type === "direct_chat_message")
        {
            return "discarded";
        }
        let received_direct_message_object = msg.message.some_json.type === "direct_chat_message" ? chat__read_message_payload(msg.message.some_json.value) : null;
        if (msg.message.some_json.type === "direct_chat_message" && received_direct_message_object == null)
        {
            return "discarded";
        }
        if (chat__receive_message_edit(received_direct_message_object, msg.message, true, sender))
        {
            return;
        }
        if (chat_reactions__receive(received_direct_message_object, msg.message, true, sender))
        {
            return;
        }
        if (sender.client != null)
        {
            chat__clear_typing_from_client(sender.client_id);
        }

        g_chat_message_author_public_keys[msg.message.server_chat_message_id] = sender.public_key;

        if (msg.message.some_json.type == "direct_chat_message")
        {
            received_direct_message_object.value = chat__sanitize_string(received_direct_message_object.value);

            let font_size1 = received_direct_message_object.font_size;

            if (!Number.isFinite(font_size1))
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

            const received_direct_message_chat_context_id = sender.context_id;
            const is_new_context = chat__get_chat_context_index_by_chat_context_id(sender.context_id) < 0;
            if (!chat__ensure_direct_context(sender))
            {
                return false;
            }
            if (is_new_context && g_are_sound_effects_enabled)
            {
                sounds__play("message_received");
            }

            let chat_context_index = chat__get_chat_context_index_by_chat_context_id(received_direct_message_chat_context_id);

            let html_to_append = "<div class=\"single-chat-message\">\n\
                                    <div class=\"single-message-content\">\n\
                                        <div class=\"single-chat-message-sender-username-container\">\n\
                                            " + android_host__generate_message_sender_html(sender.client_id, chat__sanitize_string(sender.username)) + "\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-sender-time\">\n\
                                            <p>" + new Date().toLocaleTimeString() + "</p>\n\
                                        </div>\n\
                                        <div class=\"single-chat-message-content\">\n\
                                            <p class='single-chat-message-content-p "+chat__sanitize_font(received_direct_message_object.font)+"' data-single-chat-message-server-message-id='"+ msg.message.server_chat_message_id + "' style='color: "+chat__sanitize_color(received_direct_message_object.font_color)+"; font-size: "+font_size1+";'>" + received_direct_message_object.value + "</p>\n\
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
            chat__record_direct_activity(sender, msg.message.server_chat_message_id);
            if (G_HAS_DOM)
            {
                let context = document.getElementById(received_direct_message_chat_context_id);
                chat_reactions__register(context.querySelector('[data-single-chat-message-server-message-id="' + msg.message.server_chat_message_id + '"]'), received_direct_message_object.reaction_id, sender.public_key, sender.username, received_direct_message_object.reply_to);
            }
            g_chat_context_array[chat_context_index].last_known_message_sender_username = sender.username;
        }
        // retained controls may act only on the same identity's current connection
        else if (sender.client == null)
        {
            return;
        }
        // the sender encrypted this conversation close notice for us
        else if (msg.message.some_json.type == "direct_chat_closed")
        {
            chat__show_direct_chat_closed(sender.client_id, sender.username);
        }
        // the sender encrypted this read receipt for us
        else if (msg.message.some_json.type == "message_seen")
        {
            // the eye is off, so the receipt is dropped without touching the message
            if (g_show_seen_indicator == false)
            {
                return;
            }

            chat__mark_message_as_seen(sender.client_id);
        }
        else if (msg.message.some_json.type == "channel_keys_from_maintainer")
        {
            let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, g_current_channel_id);

            // require a known maintainer before installing its keys
            if (index == -1)
            {
                console.warn("channel keys arrived for unknown channel " + g_current_channel_id + ", ignoring");
                return;
            }

            let received_keys = keys__read_channel_key_message(msg.message, g_current_channel_id, g_channel_list[index].maintainer_generation);
            if (received_keys == null)
            {
                return;
            }
            if (g_channel_list[index].has_maintainer && g_channel_list[index].maintainer_id == msg.message.sender_id)
            {
                console.log("received keys for channel '" + g_channel_list[index].name + "' from user : '" + msg.message.sender_username + "'. Setting new value of current_channel_keys");
                keys__set_current_channel_keys(received_keys);

                // do not send these keys back to the data worker, which may already have a newer set

                g_opus_decoder_worker.postMessage({
                    type: "mainthread__channel_keys_for_opus_decoder",
                    value: g_current_channel_keys
                });

                // the video worker encrypts and decrypts stream packets with the same keys
                video__post_channel_keys_to_worker();

                // stop waiting after valid maintainer keys arrive
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
     * @brief fills a private picture placeholder with the decrypted image
     *
     * @param object msg -> the server message, msg.message holds picture_id and decrypted_base64_picture
     *
     * @return boolean true when the picture placeholder exists
     */
    process_direct_chat_picture_from_server: function(msg)
    {
        chat_files__finish_chat_picture_progress(msg.message.picture_id);
        const picture = document.getElementById("chat-picture-img-" + msg.message.picture_id);
        if (picture != null)
        {
            if (!picture.classList.contains("chat-picture-deleted"))
            {
                picture.src = msg.message.decrypted_base64_picture;
                chat__scroll_chat_to_end(false);
                picture.classList.remove("chat-picture-img-default");
            }
            return true;
        }
        return false;
    },
    /**
     * @brief fills a channel picture placeholder with the decrypted image
     *
     * @param object msg -> the server message, msg.message holds picture_id and decrypted_base64_picture
     *
     * @return boolean true when the picture placeholder exists
     */
    process_channel_chat_picture_from_server: function(msg)
    {
        chat_files__finish_chat_picture_progress(msg.message.picture_id);
        const picture = document.getElementById("chat-picture-img-" + msg.message.picture_id);
        if (picture != null)
        {
            if (!picture.classList.contains("chat-picture-deleted"))
            {
                picture.src = msg.message.decrypted_base64_picture;
                picture.classList.remove("chat-picture-img-default");
                chat__scroll_chat_to_end(false);
            }
        }
        return picture != null;
    },
    /**
     * @brief completes a private file card with its decrypted content
     *
     * @param object msg -> the server message, msg.message holds file_id and the file
     *
     * @return boolean true when the card accepted the content or was already deleted
     */
    process_direct_chat_file_from_server: function(msg)
    {
        return chat_files__finish_chat_file_card(msg.message.file_id, msg.message.file);
    },
    /**
     * @brief completes a channel file card with its decrypted content
     *
     * @param object msg -> the server message, msg.message holds file_id and the file
     *
     * @return boolean true when the card accepted the content or was already deleted
     */
    process_channel_chat_file_from_server: function(msg)
    {
        return chat_files__finish_chat_file_card(msg.message.file_id, msg.message.file);
    },
    /**
     * @brief clears a failed file transfer and marks its card
     *
     * @param object msg -> the server message, msg.message.file_id names the transfer
     *
     * @return boolean true when the failed transfer has a file card
     */
    process_chat_file_decrypt_failed_from_server: function(msg)
    {
        const file_id = msg.message.file_id;
        if (!Number.isSafeInteger(file_id) || file_id < 0)
        {
            return false;
        }
        received_files = received_files.filter(file => file.file_id !== file_id);
        delete g_chat_file_transfers[file_id];
        const reason = msg.message.reason === "could not receive this file" ? msg.message.reason : "could not decrypt this file";
        const card = chat_files__get_chat_file_card_by_key(file_id);
        chat_files__mark_chat_file_card_failed(card, reason);
        return card != null;
    },
    /**
     * @brief clears a failed picture transfer and updates its placeholder
     *
     * @param object msg -> message naming the failed picture
     *
     * @return boolean -> true when the failure was applied to a picture placeholder
     */
    process_chat_picture_decrypt_failed_from_server: function(msg)
    {
        return chat_files__fail_chat_picture(msg.message.picture_id);
    },
    /**
     * @brief releases a refused upload and displays the server's reason
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
     * @brief marks the pending picture as relayed by the server
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
     * @brief moves a client to idle and stops local audio when this client idles
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
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].channel_group_id = 0;
        g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)].is_idle = true;
        keys__channel_member_left(departing_client);

        let single_client = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];
        if (!G_HAS_DOM)
        {
            return;
        }

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

                // capture the target before the event ends and currentTarget becomes null

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

                clearTimeout(local_touch_press_timer); // cancel the pending long press when the touch ends

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
                sounds__play("user_left_your_channel");
            }
        }

        // stop sending audio if it was being sent
        voice__process_stop_sending_audio();

    },
    /**
     * @brief updates a client's name in stored state and visible conversations
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
            // hidden clients have no channel row to update
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
            {
                let rename_input = document.getElementById("connected-local-client-input");
                if (rename_input != null)
                {
                    rename_input.value = msg.message.new_username;
                }
            }
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
     * @brief forgets a changed avatar and requests its replacement
     *
     * @param object msg -> the server message, msg.message.client_id names the client
     *
     * @return void
     */
    process_avatar_changed_from_server: function(msg)
    {
        if (g_server_policy.is_avatar_upload_allowed == false)
        {
            return;
        }
        let client_object = channel_tree__get_client_by_client_id(msg.message.client_id);
        if (client_object != null)
        {
            client_object.base64_avatar = null;
        }
        channel_tree__request_single_avatar(msg.message.client_id);
    },
    /**
     * @brief stores the initial client list and renders visible members
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
        if (g_is_client_list_retrieved == true)
        {
            utils__custom_log("client_list received more than once. Server is doing something weird");
            return;
        }


        g_is_client_list_retrieved = true;

        if (!G_HAS_DOM)
        {
            for (let listed of msg.message.clients)
            {
                let username = chat__sanitize_string(listed.username);
                let hidden = listed.channel_id == -1 || listed.is_idle == true ? listed.is_clients_channel_hidden : false;
                let client = messages__create_client_record(listed, username, hidden);
                g_client_list.push(client);
                g_map_client_id_to_array_index.set(client.client_id, g_client_list.length - 1);
                if (listed.channel_id != -1 && username == chat__sanitize_string(msg.message.local_username))
                {
                    g_local_username = username;
                    g_local_client_id = listed.client_id;
                }
            }
            chat__restore_peer_state();
            chat__reconcile_private_contexts();
            return;
        }

        for (let i = 0; i < msg.message.clients.length; i++)
        {
            let username = chat__sanitize_string(msg.message.clients[i].username);

            // a channel ID of -1 hides the client's location
            if (msg.message.clients[i].channel_id == -1)
            {
                let single_client = messages__create_client_record(msg.message.clients[i], username, msg.message.clients[i].is_clients_channel_hidden);

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
            }
            else if (msg.message.clients[i].is_idle == true)
            {

                let single_client = messages__create_client_record(msg.message.clients[i], username, msg.message.clients[i].is_clients_channel_hidden);

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);

                let indentation_level = 1;

                if (username != chat__sanitize_string(msg.message.local_username))
                {
                    html_to_append = channel_tree__generate_html_for_single_client(msg.message.clients[i], false);
                }
                else
                {
                    // restore the local client when the initial list places it in idle
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

                let single_client = messages__create_client_record(msg.message.clients[i], username, false);

                g_client_list.push(single_client);
                g_map_client_id_to_array_index.set(single_client.client_id, g_client_list.length - 1);
            }
        }

        chat__restore_peer_state();
        chat__reconcile_private_contexts();

        let elements = document.getElementsByClassName('connected-client');
        for (let i = 0; i < elements.length; i++)
        {
            if (g_is_client_running_under_touch_device)
            {
                let local_touch_press_timer = null; // for touch devices

                elements[i].addEventListener("touchstart", (event) => {

                    is_long_press = false;

                    // capture the target before the event ends and currentTarget becomes null

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

                    clearTimeout(local_touch_press_timer); // cancel the pending long press when the touch ends

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
        {
            let rename_input = document.getElementById('connected-local-client-input');
            if (rename_input != null)
            {
                rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout);
            }
        }

        if (g_is_running_in_android_webview == true)
        {
            Android.JavaExportOnConnected();
        }

        UI.refresh_all_channel_fullness();

        // load avatars in growing batches after the complete client list arrives
        channel_tree__enqueue_all_avatars_for_loading();

        // prefetch individual avatars when the theme has no avatar grid
        channel_tree__start_avatar_prefetch();

        // register bots before audio arrives. Node has no audio player
        if (typeof audio__audio_player_announce_music_bots === "function")
        {
            audio__audio_player_announce_music_bots();
        }

        // restore existing song titles where the server permits them to be shown
        for (let i = 0; i < msg.message.clients.length; i++)
        {
            let listed = msg.message.clients[i];

            if (listed.is_streaming_song != true)
            {
                continue;
            }
            if (g_server_policy.is_music_bot_marquee_visible_to_everyone == false && listed.channel_id != g_current_channel_id)
            {
                continue;
            }

            server_msg.process_start_song_stream_from_server({ message: { client_id: listed.client_id, song_name: listed.song_name } });
        }

        // restore the video indicator. permission to watch arrives in a separate offer
        video__apply_client_list(msg.message.clients);

        // refresh the member grid now that both policy and client data are available
        UI.refresh_member_list_state();
    },
    /**
     * @brief a channel's description, sent when this client may now read it or may no longer read it
     *
     * @param object msg -> the server message, msg.message holds channel_id, description and is_description_hidden
     *
     * @return void
     */
    process_channel_description_from_server: function(msg)
    {
        let index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, msg.message.channel_id);
        let container = document.getElementById("current-channel-description");

        // unknown channel
        if (index == -1)
        {
            return;
        }

        // store the description
        g_channel_list[index].description = msg.message.description;
        g_channel_list[index].is_description_hidden = (msg.message.is_description_hidden === true);

        // the information panel shows this channel right now: show the new text
        if (container != null && container.dataset.descriptionChannelId == String(msg.message.channel_id))
        {
            server_info__show_channel_description(g_channel_list[index]);
        }
    },
    /**
     * @brief displays and logs the server's refusal reason and permission name
     *
     * @param object msg -> the server message, msg.message.reason is the optional text, msg.message.permission the optional name of the permission or power that refused the request
     *
     * @return void
     */
    process_access_denied_from_server: function(msg)
    {
        let reason = (typeof msg.message.reason === "string" && msg.message.reason.length > 0) ? msg.message.reason : "insufficient permissions";

        // the name of the permission or power that refused the request, when the server sent one
        let permission = (typeof msg.message.permission === "string") ? msg.message.permission : "";

        // name the permission
        if (permission.length > 0)
        {
            utils__custom_alert("insufficient permissions (" + permission + ")");
            utils__custom_log("insufficient permissions (" + permission + ")", "permission_denied");
        }
        else
        {
            utils__custom_alert(reason);
            utils__custom_log(reason, "permission_denied");
        }

        // play the sound
        sounds__play("insufficient_permissions");
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
     * @brief moves the client to its new channel and updates local chat state
     *
     * @param object msg -> the server message, msg.message holds client_id and channel_id (-1 for a hidden channel)
     *
     * @return void
     */
    process_channel_join_from_server: function(msg)
    {
        let client_that_joined = g_client_list[channel_tree__get_client_index_in_array_by_client_id(msg.message.client_id)];
        let client_old_channel_id = client_that_joined.channel_id;

        // a visibility update reveals or hides the channel without moving the client
        let is_visibility_update = (msg.message.reason === "visibility");

        // a repeat of what is already known changes nothing
        if (is_visibility_update == true && client_old_channel_id === msg.message.channel_id)
        {
            return;
        }
        let departing_client = { ...client_that_joined };
        client_that_joined.channel_id = msg.message.channel_id;
        client_that_joined.channel_group_id = Number(msg.message.channel_group_id) || 0;
        groups__schedule_refresh();
        if (client_old_channel_id !== msg.message.channel_id)
        {
            keys__channel_member_left(departing_client);
        }
        keys__channel_member_joined(client_that_joined);

        // hidden destinations still announce a departure, without exposing the channel
        let local_client = channel_tree__get_client_by_client_id(g_local_client_id);
        if (local_client != null && client_old_channel_id !== msg.message.channel_id && is_visibility_update == false)
        {
            if (g_local_client_id == msg.message.client_id)
            {
                sounds__play("channel_switched");
            }
            else if (local_client.channel_id == msg.message.channel_id)
            {
                let sound = "user_joined_your_channel";
                if (msg.message.reason === "move")
                {
                    sound = "user_was_moved_to_your_channel";
                }
                else if (msg.message.reason === "kick")
                {
                    sound = "user_was_kicked_to_your_channel";
                }
                sounds__play(sound);
            }
            else if (local_client.channel_id == client_old_channel_id)
            {
                let sound = "user_left_your_channel";
                if (msg.message.reason === "move")
                {
                    sound = "user_was_moved_out_of_your_channel";
                }
                else if (msg.message.reason === "kick")
                {
                    sound = "user_was_kicked_out_of_your_channel";
                }
                sounds__play(sound);
            }
        }

        if (!G_HAS_DOM)
        {
            client_that_joined.is_clients_channel_hidden = msg.message.channel_id === -1;
            if (msg.message.client_id === g_local_client_id && msg.message.channel_id >= 0)
            {
                keys__cancel_channel_key_creation();
                g_current_channel_id = msg.message.channel_id;
                keys__set_current_channel_keys(null);
                g_current_chat_context_id = "chat-context-channel-" + msg.message.channel_id;
                g_chat_message_receiver_type = "channel";
                g_offline_chat_recipient_identity_hash = "";
                if (!g_chat_context_array.some(function(context)
                {
                    return context.chat_context_id === g_current_chat_context_id;
                }))
                {
                    g_chat_context_array.push({type: "channel", chat_context_id: g_current_chat_context_id, last_known_message_sender_username: ""});
                }
                chat__clear_channel_unread_count(msg.message.channel_id);
                for (const client of g_client_list)
                {
                    if (client.audio_state === G_AUDIO_STATE.PUSH_TO_TALK_ACTIVE)
                    {
                        client.audio_state = G_AUDIO_STATE.PUSH_TO_TALK_ENABLED;
                    }
                }
            }
            return;
        }

        // handle a hidden destination separately

        if (msg.message.channel_id == -1)
        {
            // this is case where client left to other unknown channel
            client_that_joined.is_clients_channel_hidden = true;
            if (document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]') != null)
            {
                document.querySelector('[data-connected-client-id="' + msg.message.client_id + '"]').remove();
            }
        }
        else
        {
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
                {
                    let rename_input = document.getElementById('connected-local-client-input');
                    if (rename_input != null)
                    {
                        rename_input.addEventListener("focusout", UI.connected_local_user_input_on_focusout);
                    }
                }

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

                messages__refresh_channel_context_names();

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

                // check stored conversations so this also works without rendered tabs
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

                    // read the channel name from state so it does not depend on a rendered row
                    let joined_channel = channel_tree__get_channel_by_id(g_channel_list, msg.message.channel_id);
                    let channel_name = (joined_channel != null) ? chat__sanitize_string(joined_channel.name) : "";

                    let html_to_append3 = '<div class="chat-context" id="chat-context-channel-' + msg.message.channel_id + '" style="display: none;">\n\
                                                <div class="single-server-message">now talking channel: ' + channel_name + '</div>\n\
                                                    </div>\n\
                                                </div>\n\
                                            </div>';

                    // insert after the last context when one is rendered
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
                chat__bind_private_contexts();
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

                // clear active microphone indicators until the new channel reports its speakers

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
                // move the other client's row to its new channel

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

            // restore group labels after moving the client's row

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

                // check has_icon before looking up a group icon
                let icon = (group.has_icon == true) ? channel_tree__get_icon_by_icon_id(group.group_linked_icon_id) : null;
                if (icon != null)
                {
                    node.style.backgroundImage = "url("+icon.base64_icon+")";
                }
                target_element.appendChild(node);
            }

            // the channel group icon is added together with the row, so it does not appear later than the row
            channel_groups__refresh_icons();

            let elements = document.getElementsByClassName('connected-client');

            for (let i = 0; i < elements.length; i++)
            {
                if (g_is_client_running_under_touch_device)
                {
                    let local_touch_press_timer = null; // for touch devices

                    elements[i].addEventListener("touchstart", (event) => {

                        is_long_press = false;

                        // capture the target before the event ends and currentTarget becomes null

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

                        clearTimeout(local_touch_press_timer); // cancel the pending long press when the touch ends

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
     * @brief marks a message deleted only for its recorded author or an administrator
     *
     * @param object msg -> the server message, msg.message holds chat_message_id, requester_public_key and requester_is_admin
     *
     * @return void
     */
    process_chat_message_delete_from_server: function(msg)
    {
        // check the server's requester identity against the recorded author or admin flag
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
        // a hidden sender or an early poke may have no local client record
        let poke_sender = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (poke_sender != null && poke_sender.is_ignored_by_local_client == true)
        {
            return;
        }

        if (g_are_sound_effects_enabled)
        {
            sounds__play("poke");
        }
        let poke_sender_name = channel_tree__get_display_name_by_client_id(msg.message.client_id, "");
        android_host__notify(
        {
            type: "poke",
            sender_id: msg.message.client_id,
            username: poke_sender_name,
            message: String(msg.message.poke_message || "").slice(0, 4096)
        });
        let string1 = "" + poke_sender_name + " says : " + msg.message.poke_message;
        utils__custom_alert(string1);

        // notify Android too so a background poke can reach the user
        if (g_is_running_in_android_webview == true && typeof Android !== "undefined" && g_loopback_port <= 0)
        {
            try
            {
                Android.JavaExportShowPokeNotification(poke_sender_name, msg.message.poke_message);
            }
            catch (e)
            {
                console.warn("poke notification bridge failed: " + e.message);
            }
        }
    },
    /**
     * @brief discards legacy edit commands that lack content encryption
     *
     * @param msg untrusted legacy edit command
     *
     * @return void
     */
    process_chat_message_edit_from_server: function(msg)
    {
        // accept edits only inside encrypted chat content
    },
    /**
     * @brief stores an uploaded icon and continues any remaining uploads
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
     * @brief removes a client's group from stored state and its visible row
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
        let client = channel_tree__get_client_by_client_id(msg.message.client_id);

        if (client == null)
        {
            return;
        }

        // remove the group only if it is assigned to this client

        if (client.group_ids.includes(msg.message.group_id))
        {
            client.group_ids.splice(client.group_ids.indexOf(msg.message.group_id), 1);
            if (msg.message.client_id == g_local_client_id)
            {
                sounds__play("server_group_revoked");
            }
        }

        // find the group label within this client's row
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
     * @brief updates saved contacts using their identity hashes, names and permitted public keys
     *
     * @param msg saved contact list from the server
     */
    process_stored_clients_list_from_server: function(msg)
    {
        g_offline_client_list = [];
        if (G_HAS_DOM && (!g_server_policy.is_stored_client_list_allowed || !groups__is_allowed(channel_tree__get_client_by_client_id(g_local_client_id), "view_stored_users")))
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
                // unix seconds, provided only when the server enables last seen
                last_seen: (typeof entry.last_seen === "number") ? entry.last_seen : 0,
                // the server includes the key only when both users have the required offline messaging permissions
                public_key: (typeof entry.public_key === "string") ? entry.public_key : ""
            });
        }

        utils__custom_log("stored clients list: " + g_offline_client_list.length + " people kept of " + msg.message.stored_clients.length + " sent");
        UI.schedule_member_list_sync();
    },


    /**
     * @brief updates the country code displayed for a client
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

        // refresh live flags before the member list copies these rows
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
     * @brief adds a client's group and updates the available controls
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

        // resolve the group before adding its ID to the client
        if (group == null)
        {
            console.log("group is null");
            return;
        }

        if (!client.group_ids.includes(msg.message.group_id))
        {
            client.group_ids.push(msg.message.group_id);
            if (msg.message.client_id == g_local_client_id)
            {
                sounds__play("server_group_assigned");
            }
        }

        let target_element = document.getElementById("client-groups-" + msg.message.client_id);

        // hidden clients have no row. continue updating their permissions
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
     * @brief adds a group received from the server
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
     * @brief removes a deleted group from the list and from connected users
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
            if (g_groups[i].group_id == group_id)
            {
                g_groups.splice(i, 1);
            }
        }

        let displayed_groups = document.querySelectorAll('.single-group[group-id="' + group_id + '"]');
        for (let i = 0; i < displayed_groups.length; i++)
        {
            displayed_groups[i].remove();
        }

        for (let i = 0; i < g_client_list.length; i++)
        {
            let index_of_group = g_client_list[i].group_ids.indexOf(group_id);
            if (index_of_group != -1)
            {
                g_client_list[i].group_ids.splice(index_of_group, 1);
            }
        }
    },
    /**
     * @brief removes a deleted server icon and updates the places that displayed it
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
     * @brief updates a channel icon in state, its row and any open edit form
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
     * @brief updates the icon shown beside users with this group
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
     * @brief displays a music bot's songs and removal controls
     *
     * @param object msg -> the server message, msg.message.songs holds the songs
     *
     * @return void
     */
    process_music_bot_song_list_from_server: function(msg)
    {
        moderation__music_controls(msg.message);

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
            document.getElementsByClassName("remove-song-from-musicbot-button")[x].disabled = !groups__has_permission("delete_music_tracks");
            document.getElementsByClassName("remove-song-from-musicbot-button")[x].onclick = function(event) {
                event.stopPropagation();

                let song_id = parseInt(event.currentTarget.getAttribute("data-song-id"));

                client_msg.send_remove_song_from_music_bot_request(song_id, parseInt(g_selected_client_id));
            };
        }
    },

    /**
     * @brief completes the current upload and sends its pending file intent once
     *
     * @param object msg -> the server message
     *
     * @return void
     */
    process_file_send_success_from_server: function(msg)
    {
        let was_picture = (g_file_send_intent == "direct_chat_picture_file" || g_file_send_intent == "channel_chat_picture_file");

        // release the upload lock before checking its completion intent
        chat__release_file_upload_lock();

        if (g_file_send_intent == "musicbot_file" || g_file_send_intent == "direct_chat_picture_file" || g_file_send_intent == "channel_chat_picture_file" || g_file_send_intent == "direct_chat_file" || g_file_send_intent == "channel_chat_file")
        {
            client_msg.send_file_send_completed_request(g_file_send_intent);

            // clear the intent so duplicate acknowledgements cannot submit it again
            g_file_send_intent = "";
            g_file_send_intent_extra_data = {};
        }

        // image_sent_status confirms the picture's relay after upload
        if (was_picture == true)
        {
            chat__show_picture_delivery_status();
        }
    },

    /**
     * @brief stores an incoming file chunk and refreshes its receive timeout
     *
     * @param object msg -> the server message, msg.message holds server_chat_message_id and the chunk
     *
     * @return void
     */
    process_file_receive_chunk_from_server_from_server: function(msg)
    {
        chat_files__expire_receiving();
        const id = msg.message.server_chat_message_id;
        const file = received_files.find(item => item.file_id === id);
        const chunk = msg.message.value;
        if (!file)
        {
            return;
        }
        if (typeof chunk !== "string" || chunk.length === 0 || chunk.length > 65536 || file.file_content_base64.length + chunk.length > file.expected_size)
        {
            chat_files__discard_receiving(id);
            return;
        }
        file.timestamp_last_received = Date.now();
        file.file_content_base64 += chunk;
        chat_files__update_chat_file_progress(id, file.file_content_base64.length);
    },

    /**
     * @brief submits a complete received file to the data worker for decryption
     *
     * @param object msg -> the server message, msg.message.server_chat_message_id names the file
     *
     * @return boolean|string|undefined false for an incomplete transfer, "deferred" while local decryption finishes, otherwise undefined
     */
    process_file_receive_completed_from_server_from_server: function(msg)
    {
        chat_files__expire_receiving();
        const id = msg.message.server_chat_message_id;
        const index = received_files.findIndex(file => file.file_id === id);
        if (index < 0)
        {
            return false;
        }
        const file = received_files[index];
        if (file.file_content_base64.length !== file.expected_size)
        {
            chat_files__discard_receiving(id);
            return false;
        }
        const message_raw = file.file_content_base64;
        received_files.splice(index, 1);

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
                sender_id: msg.message.sender_id,
                local_delivery: msg.local_delivery,
                connection_generation: g_worker_connection_generation
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
                sender_id: msg.message.sender_id,
                local_delivery: msg.local_delivery,
                connection_generation: g_worker_connection_generation
            });
        }
        if (msg.local_delivery && (msg.message.receive_type === "channel_chat_file" || msg.message.receive_type === "channel_chat_picture"))
        {
            return "deferred";
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
     * @brief requests stored users only when the server enables the list and this client may view it
     *
     * @return void
     */
    send_request_stored_clients: function()
    {
        let client = channel_tree__get_client_by_client_id(g_local_client_id);
        if (!g_is_client_list_retrieved || g_fast_reconnect.resumed || !g_server_policy.is_stored_client_list_allowed || !groups__is_allowed(client, "view_stored_users"))
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
     * @brief uploads one file part using the current socket
     *
     * @param number total_bytes_length -> the whole file's length
     * @param string data_part_base64 -> this part
     * @param number index -> the part's index
     *
     * @return Promise<boolean> -> whether the current socket accepted this chunk
     */
    send_file_send_request: function(total_bytes_length, data_part_base64, index)
    {
        let is_new_file = false;
        if (index === 0)
        {
            is_new_file = true;
        }

        let message_object = {
            message: {
                type: "file_send",
                total_bytes_length: total_bytes_length,
                data_part_base64: data_part_base64,
                is_new_file: is_new_file,
                file_send_intent: index === 0 ? g_file_send_intent : undefined,
                file_send_intent_extra_data: index === 0 ? g_file_send_intent_extra_data : undefined
            }
        };

        return connection__send_upload_message(message_object);
    },
    /**
     * @brief tells the server how to use the completed upload and its extra data
     *
     * @param string g_file_send_intent -> the intent, "musicbot_file", "channel_chat_file" and the like
     *
     * @return void
     */
    send_file_send_completed_request: function(g_file_send_intent)
    {
        // send the intent after the server confirms the complete upload

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
