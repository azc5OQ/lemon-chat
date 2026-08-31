var UI = {

    // removes every open channel-list / add-tag / chat-input context menu; setbackgroundColor
    // also clears the row highlights, and clicks on add-tag menu items are left alone
    delete_contextmenus: function(setbackgroundColor = false, handle_tag_contextmenu_click = true)
    {
        // quick fix
        if (handle_tag_contextmenu_click)
        {
            if (event != null && event.target != null)
            {
                if (event.target.classList.contains("add-tag-context-menu-item"))
                {
                    return;
                }
            }
        }

        let elements_count1 = document.getElementsByClassName("channel-list-context-menu").length;
        for (let i = 0; i < elements_count1; i++)
        {
            document.getElementsByClassName("channel-list-context-menu")[0].remove();
        }

        let elements_count2 = document.getElementsByClassName("add-tag-context-menu").length;
        for (let i = 0; i < elements_count2; i++)
        {
            document.getElementsByClassName("add-tag-context-menu")[0].remove();
        }

        let elements_count3 = document.getElementsByClassName("chat-input-container-context-menu").length;
        for (let i = 0; i < elements_count3; i++)
        {
            document.getElementsByClassName("chat-input-container-context-menu")[0].remove();
        }

        if (setbackgroundColor == true)
        {
            let elements = document.getElementsByClassName('single-channel');
            for (let i = 0; i < elements.length; i++)
            {
                elements[i].style.backgroundColor = "";
            }

            let elements1 = document.getElementsByClassName('connected-client');
            for (let i = 0; i < elements1.length; i++)
            {
                elements1[i].style.backgroundColor = "";
            }
        }
    },

    // removes every chat-message context menu and drops the hover class from local message rows
    delete_chat_message_contextmenu: function()
    {
        // delete chat message contextmenus
        let elements_count = document.getElementsByClassName("chat-message-context-menu").length;

        for (let i = 0; i < elements_count; i++)
        {
            document.getElementsByClassName("chat-message-context-menu")[0].remove();
        }

        let elements_count1 = document.getElementsByClassName("local-single-chat-message-content-p").length;

        for (let i = 0; i < elements_count1; i++)
        {
            document.getElementsByClassName("local-single-chat-message-content-p")[i].classList.remove("single-chat-message-content-p-hover");
        }
    },

    // expand/collapse arrow on a channel row: flips is_channel_directly_collapsed in
    // g_channel_list and toggles .collapsed on all subchannel rows and their client rows
    collapse_expand_channel: function(e)
    {
        event.stopPropagation();

        UI.delete_contextmenus(true);

        if (event.which != 1)
        {
            return;
        }

        let element = e.target.parentNode;

        let channel_id = parseInt(element.getAttribute("data-channel-id"));

        if (e.target.classList.contains("single-channel-expand-button"))
        {
            e.target.classList.remove("single-channel-expand-button");

            let index = get_channel_index_in_array_by_channel_id(g_channel_list, channel_id);
            g_channel_list[index].is_channel_directly_collapsed = false;

            let client_ids = [];
            let channel_ids = [];

            channel_ids = find_subchannels_of_channel_to_expand(channel_id);

            console.log(channel_ids);

            for (let i = 0; i < channel_ids.length; i++)
            {
                document.querySelector('[data-channel-id="' + channel_ids[i] + '"]').classList.remove("collapsed");
            }

            channel_ids.push(channel_id);

            client_ids = find_clients_in_channel(channel_ids);

            for (let i = 0; i < client_ids.length; i++)
            {
                document.querySelector('[data-connected-client-id="' + client_ids[i] + '"]').classList.remove("collapsed");
            }
        }
        else
        {
            e.target.classList.add("single-channel-expand-button");

            let index = get_channel_index_in_array_by_channel_id(g_channel_list, channel_id);
            g_channel_list[index].is_channel_directly_collapsed = true;

            let client_ids = [];
            let channel_ids = [];

            channel_ids = find_subchannels_of_channel_for_collapse(channel_id);

            for (let i = 0; i < channel_ids.length; i++)
            {
                document.querySelector('[data-channel-id="' + channel_ids[i] + '"]').classList.add("collapsed");
            }

            channel_ids.push(channel_id); // so clients in current channel are also hidden

            for (let i = 0; i < channel_ids.length; i++)
            {
                let channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, channel_ids[i]);
            }

            client_ids = find_clients_in_channel(channel_ids);

            for (let i = 0; i < client_ids.length; i++)
            {
                document.querySelector('[data-connected-client-id="' + client_ids[i] + '"]').classList.add("collapsed");
            }
        }
    },

    // left click on a channel row: just closes any open context menus and their row highlights
    single_channel_onclick: function(event)
    {
        event.stopPropagation();
        UI.delete_contextmenus(true);
    },

    // double click joins the channel: opens the password dialog if it needs one, otherwise
    // sends join_channel_request straight away (no-op for the current channel)
    single_channel_doubleclick_join: function(event)
    {
        event.stopPropagation();
        if (typeof window === 'undefined')
        {
            return;
        }

        let channel_node = event.target.parentNode;

        UI.delete_contextmenus(true);

        let selected_channel_id = parseInt(channel_node.getAttribute("data-channel-id"));

        if (selected_channel_id == current_channel_id)
        {
            console.log("cannot join current channel"); // checked also at server side
            return;
        }

        let selected_channel = get_channel_by_id(g_channel_list, selected_channel_id);

        if (selected_channel.is_using_password == true)
        {
            document.getElementById("channel-password-enter-container").style.display = "block";
            document.getElementById("background-container").style.display = "block";
        }
        else
        {
            let message_object = {
                message: {
                    type: "join_channel_request",
                    channel_id: parseInt(selected_channel_id),
                    channel_password: ""
                }
            };

            console.log(message_object);

            send_message_object(message_object);
            console.log("join channel requested: channel_id=" + selected_channel_id);
        }
    },

    // a chat tab was clicked: shows that context, highlights the tab, updates the receiver
    // globals (g_current_chat_context_id etc) and clears a user tab's unread counter
    chat_context_selector_onclick: function()
    {
        let id_to_find = "";
        let client_id = "";
        let channel_id = "";
        let selector_type = this.getAttribute("data-chat-context-selector-type");

        for (let i = 0; i < document.getElementsByClassName("chat-context-selector").length; i++)
        {
            document.getElementsByClassName("chat-context-selector")[i].style.backgroundColor = "";
        }

        let elements1 = document.getElementsByClassName('connected-client');
        for (let i = 0; i < elements1.length; i++)
        {
            elements1[i].style.backgroundColor = "";
        }

        this.style.backgroundColor = "#36393f";

        for (let i = 0; i < document.getElementsByClassName("chat-context").length; i++)
        {
            document.getElementsByClassName("chat-context")[i].style.display = "none";
        }

        if (selector_type == "user")
        {
            UI.enable_inputs();
            id_to_find = "chat-context-pm-";
            client_id = this.getAttribute("data-chat-context-selector-id").split("user-")[1];
            id_to_find += client_id;

            document.getElementById(id_to_find).style.display = "block";

            // opening the conversation is what marks it read, so the count is state.
            // render_unread_badge no-ops when the row is not painted, which is what the
            // old custom_typeof guard was reaching for
            clear_unread_count(client_id);
            render_unread_badge(client_id, false);

            g_current_chat_context_id = id_to_find;
            console.log("current_chat_context_id -> ", g_current_chat_context_id);

            g_chat_message_receiver_type = "user"; // from local user perspective
            chat_message_receiver_id = client_id; // from local user perspective
        }
        else if (selector_type == "channel")
        {
            id_to_find = "chat-context-channel-";
            channel_id = this.getAttribute("data-chat-context-selector-id").split("channel-")[1];
            id_to_find += channel_id;

            console.log("trying to find id - > ", id_to_find)
            g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_alias = ""; /* back on a channel: no offline target */ // from local user perspective

            let is_found = document.getElementById(id_to_find);
            if (is_found)
            {
                document.getElementById(id_to_find).style.display = "block";
            }

            if (current_channel_id == channel_id)
            {
                UI.enable_inputs();
                g_current_chat_context_id = id_to_find;
                console.log("changed current_chat_context_id to -> " + id_to_find);
            }
            else
            {
                UI.disable_inputs();
            }
        }

        // the member strip marks the on-screen conversation with a ring on its circle
        UI.schedule_member_list_sync();

        document.getElementById('chat-context-container').scrollTop = document.getElementById('chat-context-container').scrollHeight;
    },

    // re-enables the chat text input, send button and image upload and shows the font/color controls
    enable_inputs: function()
    {
        console.log("function enable inputs()");
        document.getElementById("chat-input-container-text-input").disabled = false;
        document.getElementById("chat-input-container-send-chat-input").disabled = false;
        document.getElementById("choose_image_input").disabled = false;
        document.getElementById("choose-file-input").disabled = false;
        document.getElementById("color-picker-selector-div").style.display = "block";
        document.getElementById("chat-input-font-size-range").style.display = "block";
    },

    // disables the chat text input, send button and image upload and hides the font/color controls
    disable_inputs: function()
    {
        console.log("function disable_inputs()");
        document.getElementById("chat-input-container-text-input").disabled = true;
        document.getElementById("chat-input-container-send-chat-input").disabled = true;
        document.getElementById("choose_image_input").disabled = true;
        document.getElementById("choose-file-input").disabled = true;
        document.getElementById("color-picker-selector-div").style.display = "none";
        document.getElementById("chat-input-font-size-range").style.display = "none";
    },

    // a tag row in the add-tag submenu was clicked: sends add_tag_to_client or
    // remove_tag_from_client for selected_client_id depending on its current state
    add_tag_contextmenu_onclick: function(event)
    {
        event.stopPropagation();

        let tag_id = parseInt(event.srcElement.getAttribute("data-tag-id"));
        let is_tag_active_right_now = event.srcElement.classList.contains("context-menu-item-tag-active");

        if (is_tag_active_right_now)
        {
            let message_object = {
                message: {
                    type: "remove_tag_from_client",
                    tag_id: tag_id,
                    client_id: parseInt(selected_client_id)
                }
            };

            send_message_object(message_object);

            UI.delete_contextmenus(false, false);
        }
        else
        {
            let message_object = {
                message: {
                    type: "add_tag_to_client",
                    tag_id: tag_id,
                    client_id: parseInt(selected_client_id)
                }
            };

            send_message_object(message_object);

            UI.delete_contextmenus(false, false);
        }
    },

    // hovering the "add tag" menu item: highlights it and reveals the add-tag submenu
    add_tag_onmouseenter: function(event)
    {
        event.stopPropagation();
        event.target.style.backgroundColor = "#b9d436";
        let element = document.getElementsByClassName('add-tag-context-menu');
        if (element != null)
        {
            element[0].style.display = "block";
        }
    },

    // leaving "add tag" towards another menu item: hides the submenu and clears the highlight
    add_tag_onmouseleave: function(event)
    {
        event.stopPropagation();

        if(event.relatedTarget != null && event.relatedTarget.classList.contains("context-menu-item"))
        {
            let element = document.getElementsByClassName('add-tag-context-menu');
            if (element != null)
            {
                element[0].style.display = "none";
            }

            let element1 = document.querySelector('.context-menu-item[data-action="6"]');
            if (element1 != null)
            {
                element1.style.backgroundColor = "";
            }
        }
    },

    // mic button: on desktop the checkbox turns the microphone on/off (sends microphone_usage,
    // stops the audio tracks); on touch devices touchstart/touchend act as push-to-talk
    toggle_microphone_onclick: function(event)
    {
        // so workers ignore this function
        if (typeof window === 'undefined')
        {
            return;
        }

        // if this got clicked on desktop
        // or if this got touched on touchscreen device (only android supported)
        if (event.currentTarget.checked || (g_is_microphone_enabled_on_touch_device == false && g_is_client_running_under_touch_device == true))
        {
            activate_microphone();

            // desktop, continuous mode: turning the mic on IS the start of talking,
            // and it keeps going until it is turned off again. push-to-talk instead
            // leaves it silent here and speaks only while the key is held
            if (g_is_continuous_mic_mode == true && g_is_client_running_under_touch_device == false)
            {
                g_is_continuous_transmission_active = true;
                process_start_sending_audio();
            }
        }
        else if (event.currentTarget.checked == false && g_is_client_running_under_touch_device == false)
        {
            // continuous mode was transmitting: stop before the mic goes away
            if (g_is_continuous_transmission_active == true)
            {
                g_is_continuous_transmission_active = false;
                process_stop_sending_audio();
            }

            document.getElementById("custom-file-upload-button-song").style.visibility = "hidden";
            document.getElementById("stop-song").style.display = "none";

            let message_object = {
                message: {
                    type: "microphone_usage",
                    value: 3,
                }
            };

            let message_json_string = process_message_before_sending(message_object);
            let data = encrypt_all_message_data_and_convert_to_base64(message_json_string);

            websocket_worker_send(data);
            g_last_sent_value_microphone_usage = 2;

            g_is_microphone_enabled = false;

            if (g_local_audio_stream != null)
            {
                if (g_local_audio_stream.getTracks() != null)
                {
                    g_local_audio_stream.getTracks().forEach(function (track)
                    {
                        track.stop();
                    });
                }
            }

            document.getElementById("toggle-microphone-label").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAYAAABXAvmHAAAELklEQVRoQ+2ZWehOXRTGf+hDuJIpmVKUJIUiY9wRMpRMFy58hogoZUgk04WUqQyJEqJMZSgy9hlKXBK5+MgsuVDm0KN9tJ3/Gfbe55z3f2PdvO85Z++1n2fvtddae+0GlC/DgOmAftsDDYHnwDVgL3C9zCEblKisFbAPGJ2j8xTwL/CmjLHLItAFuAh0dQT1PzAc0G8hKYNAC+AG0MsTyX2gH/DBs98fzcsgsA5YHghiPbAisO+vbkUJNAdeAfoNkRdAB+B7QufWwDjgYNYqFSUwFtCmDBGZjjb85YTObc37HuZX7RJNrSiBZYDMwFdcwUd6RTKRRFECm4FFnuh9wUfqZwF74mMVJbAFWOBBIBS8PJYCY53YUUsCAj8GuJRAuI153zPh2wMTM7Th60itCFQCvgw36mJClYGvBYFKwVdNoHLwVRKoCfiqCNQMvCuBf4AdQB9gFXDG8mXxTRzq5+8BI0xepYi7GrgLzAO+ZsUZFzc6BThklCiQyGdHYhMIBS8/ryClpFCiMXQ4kkwFDhcloEgroJHYpCMCoWYjnVuBhZb+H9Z/vdf3VHFZgTwCMwMjbASqXglsBM4HpAf2jFZOYDaw0xpRm/qbeW6Wkqfb+XyWBejbJmCJadQY+Gx1mBsbu44uFxPSqeiE1VPgXmeg8gEvNQIvEpKOwBNL90TgeNE90B+4ZSmRx1CNJ0l8wUuH3Gd0KhsA3LQUDwH+K0qgpZnxRsZcpFQ+Oi4h4LWSOhNHvn4psMEo1jm5XV79yMWEpE8z3tfxDJtn8/Z3ebht1gtV7Qaa5zum7JKpz5WAIqKiZd4B3Af8BWCU5RAEXOYSYXIqubgSSAMWYjbSpUKYUoZ3RrFwaJUHm+cvQLfYhk7EUIRACHgBk8momGW7SxXGVCCLZBcwx2U5QwlkgdcBfD8wCOgMKDV4amb4KPA4BkyuUu9VxZa8BXoDz6oiEHwATwCkUsl2QMFRIrLjfYplviuQNfPxrDJrArUySkMmxxqtBVa6zHzUxodAns3LvmUKB4ArgJ5tUZqgIDgBmAE0jX3XWWOND3i1dSWQBz4+rgLTQxMAZR4yu04JoNXvI7A4L+dJI+ZCIG/D6pJipO/MmfYqcilZfBTYP3cF8sDrlkUnqaHAfEDV6iY5YOQ+laDtBq6ajRuKP5OAK3h7cN0TyM7lBuXHZTYSuU7l/UoPlEe9D0Yc65hmQrpc0OyoPh8X+flo5rNwnLVM65xJG8rC/VtPGgHdImqJQ8GrX70S0EnrtJnpiITrzEft65WAQNgkksDLZ0+yomh8tZTLS4dEVYuXKfYjl3vE1Jy8TSzPjQrANOBk7GCh69Hb3qNld5BObXIvySOQpkwX20odFF3LEEXt7gmJXq7uUAJSHKUF8ZQgd9BYg0/AsYxzdqa+IgR8gVbS/i+BSqbVQ+lPvaMNQOE4cEcAAAAASUVORK5CYII=)";
        }
        else if (g_is_microphone_enabled_on_touch_device == true && g_is_client_running_under_touch_device == true && event.type == "touchstart")
        {
            // audio is not available, so say so rather than looking broken
            if (g_is_microphone_available == false)
            {
                custom_alert("the voice connection is not established, so the microphone cannot be used yet");
                return;
            }

            let ptt_button = document.getElementById("microphone-push-to-talk-button-touch-device");

            // continuous mode: one tap starts, the next tap stops - the button shows which
            if (g_is_continuous_mic_mode == true)
            {
                if (g_is_continuous_transmission_active == true)
                {
                    g_is_continuous_transmission_active = false;
                    ptt_button.classList.remove("mic-continuous-active");
                    process_stop_sending_audio();
                }
                else
                {
                    g_is_continuous_transmission_active = true;
                    ptt_button.classList.add("mic-continuous-active");
                    process_start_sending_audio();
                }

                return;
            }

            ptt_button.classList.add("ptt-pressed");
            process_start_sending_audio();
        }
        else if (g_is_microphone_enabled_on_touch_device == true && g_is_client_running_under_touch_device == true && event.type == "touchend")
        {
            // in continuous mode the toggle already happened on touchstart
            if (g_is_continuous_mic_mode == true)
            {
                return;
            }

            document.getElementById("microphone-push-to-talk-button-touch-device").classList.remove("ptt-pressed");
            process_stop_sending_audio();
        }
    },

    // the x on a chat tab: removes that context's html and g_chat_context_array entry, then
    // falls back to the current channel's context (the current channel itself can't be removed)
    chat_context_remove_onclick: function(event)
    {
        if (typeof window === 'undefined')
        {
            return;
        }
        event.stopPropagation();

        let selector_type = event.srcElement.getAttribute("data-chat-context-remove-selector-type");

        if (selector_type == "user")
        {
            let selector_id = this.getAttribute("data-chat-context-remove-selector-id");
            let client_id = selector_id.split("user-")[1];
            document.querySelector('[data-chat-context-selector-id="' + selector_id + '"]').remove();
            document.getElementById("chat-context-pm-" + client_id).remove();

            let chat_context_index_to_remove = get_chat_context_index_by_chat_context_id("chat-context-pm-" + client_id);

            if (chat_context_index_to_remove != -1)
            {
                g_chat_context_array.splice(chat_context_index_to_remove, 1);
            }

            document.getElementById("chat-context-channel-" + current_channel_id).style.display = "block";
            g_current_chat_context_id = "chat-context-channel-" + current_channel_id;
            clear_channel_unread_count(current_channel_id); // opened it, so it is read
            g_chat_message_receiver_type = "channel";
            g_offline_chat_recipient_alias = ""; // back on a channel: no offline target
            UI.schedule_member_list_sync(); // active ring moves back to the channel circle

            for (let i = 0; i < document.getElementsByClassName("chat-context-selector").length; i++)
            {
                document.getElementsByClassName("chat-context-selector")[i].style.backgroundColor = "";
            }

            let elements1 = document.getElementsByClassName('connected-client');
            for (let i = 0; i < elements1.length; i++)
            {
                elements1[i].style.backgroundColor = "";
            }

            document.querySelector('[data-chat-context-selector-id="channel-' + current_channel_id + '"]').style.backgroundColor = "#36393f";
        }
        else if (selector_type == "channel")
        {
            // cannot remove current channel

            let selector_id = this.getAttribute("data-chat-context-remove-selector-id");
            let channel_id = selector_id.split("channel-")[1];

            console.log("current_channel_id " + current_channel_id);
            console.log("channel_id " + channel_id);

            if (parseInt(current_channel_id) == parseInt(channel_id))
            {
                custom_alert("current channel, can't remove");
                return;
            }

            document.querySelector('[data-chat-context-selector-id="' + selector_id + '"]').remove();
            document.getElementById("chat-context-channel-" + channel_id).remove();

            let chat_context_index_to_remove = get_chat_context_index_by_chat_context_id("chat-context-channel-" + channel_id);

            if (chat_context_index_to_remove != -1)
            {
                g_chat_context_array.splice(chat_context_index_to_remove, 1);
            }

            document.getElementById("chat-context-channel-" + current_channel_id).style.display = "block";
            g_current_chat_context_id = "chat-context-channel-" + current_channel_id;
        clear_channel_unread_count(current_channel_id); // opened it, so it is read
            document.querySelector('[data-chat-context-selector-id="channel-' + current_channel_id + '"]').style.backgroundColor = "#36393f";

            UI.enable_inputs();
            UI.schedule_member_list_sync(); // active ring moves back to the channel circle
        }
    },

    // click on a user row: left click opens (or creates) the private chat with him and fills
    // the right-pane profile; right click builds the per-user context menu (mute, tags, kick..)
    connected_user_onmousedown: function(event, is_touch_event = null, touch_clientX = null, touch_clientY = null, touch_current_target = null, is_short_click = false)
    {
        // this function "connected_user_onmousedown" is appended to connected-client html element in these functions
        // function process_channel_join_from_server
        // function process_client_connect_from_server
        // function process_client_list_from_server

        if (event && typeof event.stopPropagation === "function")
        {
            event.stopPropagation();
        }

        let clientX = 0;
        let clientY = 0;
        let currentTarget = null;
        let is_this_idle_client = false;
        let is_this_music_bot_client = false;

        if (event.currentTarget == null)
        {
            currentTarget = touch_current_target;
        }
        else
        {
            currentTarget = event.currentTarget;
        }

        if (is_touch_event == null)
        {
            clientX = event.clientX;
            clientY = event.clientY;
        }
        else
        {
            clientX = touch_clientX;
            clientY = touch_clientY;
        }

        if (currentTarget.classList.contains("idle-client"))
        {
            is_this_idle_client = true;
        }

        if (currentTarget.classList.contains("music-bot-client"))
        {
            is_this_music_bot_client = true;
        }

        if (event.which == 1 || is_touch_event == true && is_short_click == true)
        {
            UI.delete_contextmenus();
            // remove color highlight from other clients

            document.getElementById("current-channel-description").innerHTML = "";
            for (let i = 0; i < document.getElementsByClassName('connected-client').length; i++)
            {
                document.getElementsByClassName('connected-client')[i].style.backgroundColor = "";
            }

            chat_message_receiver_id = currentTarget.getAttribute("data-connected-client-id");

            if (chat_message_receiver_id != local_client_id)
            {
                // change color of chat context selector only if client that is not local client is selected

                for (let i = 0; i < document.getElementsByClassName("chat-context-selector").length; i++)
                {
                    document.getElementsByClassName("chat-context-selector")[i].style.backgroundColor = "";
                }

                g_chat_message_receiver_type = "user";

                // alias when he has one: this labels the chat header and the pill, and
                // "now talking to user: user0" for somebody everyone knows as fred was
                // the whole confusion
                let data_username = get_display_name_by_client_id(chat_message_receiver_id);
                let id_to_find = "chat-context-pm-" + chat_message_receiver_id;
                g_current_chat_context_id = id_to_find;
                g_offline_chat_recipient_alias = ""; // this person is connected: normal direct path
                UI.schedule_member_list_sync(); // ring the opened chat's circle in the strip
                let is_found = document.getElementById(id_to_find);

                if (is_found == null)
                {
                    // make all elements invisible
                    for (let i = 0; i < document.getElementsByClassName("chat-context").length; i++)
                    {
                        document.getElementsByClassName("chat-context")[i].style.display = "none";
                    }

                    let html_to_append = "<div class=\"chat-context\" id=\"" + id_to_find + "\">\n\
                                                    <div class=\"single-server-message\">now talking to user: " + data_username + "</div>\n\
                                                    <div class=\"single-server-message\">your public key: " + g_rsa_public_key_string + "</div>\n\
                                                    <div class=\"single-server-message\">his public key: " + sanitize_string(get_public_key_by_client_id(chat_message_receiver_id)) + "</div>\n\
                                                    <div class=\"single-server-message\"> </div>\n\
                                                </div>";

                    document.getElementById('chat-context-container').insertAdjacentHTML("beforeend", html_to_append);

                    let single_chat_context = {
                        type: "user",
                        chat_context_id: g_current_chat_context_id,
                        last_known_message_sender_username: ""
                    };

                    g_chat_context_array.push(single_chat_context);
                }
                else
                {
                    for (let i = 0; i < document.getElementsByClassName("chat-context").length; i++)
                    {
                        document.getElementsByClassName("chat-context")[i].style.display = "none";
                    }

                    document.getElementById(id_to_find).style.display = "block";

                    // opening the conversation marks it read
                    clear_unread_count(chat_message_receiver_id);
                    render_unread_badge(chat_message_receiver_id, false);
                }
                is_found = document.querySelector('[data-chat-context-selector-id="user-' + chat_message_receiver_id + '"]');

                if (is_found == null)
                {
                    let to_append = "<div class=\"chat-context-selector\" data-chat-context-selector-type=\"user\" data-chat-context-selector-id=\"user-" + chat_message_receiver_id + "\">\n\
                                                <div class=\"p-container\">\n\
                                                    <p>" + data_username + "</p>\n\
                                                </div>\n\
                                                <div class=\"remove-chat-context-selector\" data-chat-context-remove-selector-type=\"user\" data-chat-context-remove-selector-id=\"user-" + chat_message_receiver_id + "\">\n\
                                                </div>\n\
                                            </div>";

                    document.getElementById("chat-context-selectors-container").insertAdjacentHTML("beforeend", to_append);
                }

                for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
                {
                    document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
                }

                for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
                {
                    document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
                }

                document.querySelector('[data-chat-context-selector-id="user-' + chat_message_receiver_id + '"]').style.backgroundColor = "#36393f";

                UI.enable_inputs();
            }

            // remember which client the right-pane profile shows, so its avatar (if any) lands in
            // the big #current-client-avatar when the response arrives
            g_profile_avatar_client_id = parseInt(chat_message_receiver_id);

            let message_object = {
                message: {
                    type: "request_avatar_for_client",
                    client_id: parseInt(chat_message_receiver_id)
                }
            };

            send_message_object(message_object);

            // whether we clicked on local client or not, still add g_tags to description..

            document.getElementById("current-client-description").style.display = "block";
            document.getElementById("current-channel-description").style.display = "none";
            let client = get_client_by_client_id(chat_message_receiver_id);
            document.getElementById("current-client-description-client-name").innerHTML = sanitize_string(client.username);
            document.getElementById("current-client-description-tags").innerHTML = "";
            document.getElementById("current-client-avatar").style.backgroundImage = "";
            document.getElementById("current-client-avatar").style.backgroundSize = "";
            document.getElementById("current-client-avatar").style.width = "";
            document.getElementById("current-client-avatar").style.height = "";

            // the avatar box is a fixed circle, so with no picture it was a hole in the card.
            // it holds the first letter until the requested avatar arrives, or instead of it
            set_profile_avatar_monogram(client.username);

            document.getElementById("current-client-description-meta").textContent =
                describe_client_for_profile(client);

            for (let i = 0; i < client.tag_ids.length; i++)
            {
                let tag = get_tag_by_tag_id(client.tag_ids[i]);

                if (tag == null)
                {
                    continue;
                }

                // same has_icon guard as the tag rows, plus a null check - this dereferenced the
                // icon unchecked, so a tag pointing at a deleted icon threw here
                let icon = (tag.has_icon == true) ? get_icon_by_icon_id(tag.tag_linked_icon_id) : null;
                let icon_style = (icon != null) ? ("background-image: url(" + icon.base64_icon + ")") : "";

                let html_to_append = '<div class="client-description-tag-entry">\n\
                                        <div class="client-description-tag-entry-icon" style="'+icon_style+'">\n\
                                        </div>\n\
                                        <div class="client-description-tag-entry-name">'+tag.tag_name+'\n\
                                        </div>\n\
                                    </div>';

                document.getElementById("current-client-description-tags").insertAdjacentHTML("beforeend", html_to_append);
            }
        }
        else if (event.which == 3 || is_touch_event == true && is_short_click == false)
        {
            selected_client_id = currentTarget.getAttribute("data-connected-client-id");
            console.log("selected_client_id : " + selected_client_id);

            let elements = document.getElementsByClassName('connected-client');

            for (let i = 0; i < elements.length; i++)
            {
                elements[i].style.backgroundColor = "";
            }

            let elements1 = document.getElementsByClassName('single-channel');
            for (let i = 0; i < elements1.length; i++)
            {
                elements1[i].style.backgroundColor = "";
            }

            currentTarget.style.backgroundColor = "var(--row-rightclick-selection, #333030)";

            UI.delete_contextmenus();

            // create two divs one visible, second invisible, becomes visible after hover on "add tag". tags will be shown in it

            let add_tag_html_class = "";
            if (is_local_client_admin() == false)
            {
                add_tag_html_class = " context-menu-item-disabled";
            }

            // admin-only and only when the server allows alias registrations
            let set_alias_html_class = " context-menu-item-disabled";
            if (is_local_client_admin() == true && g_is_alias_registration_allowed == true)
            {
                set_alias_html_class = "";
            }

            let contextmenu_useronclick = "";

            if (selected_client_id != local_client_id)
            {
                let the_client = get_client_by_client_id(selected_client_id);

                let string_to_append = "";

                if (is_local_client_admin() && the_client.is_music_bot == false)
                {
                    string_to_append = "<p class='context-menu-item' data-action='12'>kick</p>\n\
                                        <p class='context-menu-item' data-action='13'>ban</p>";
                }

                if (is_this_idle_client == true)
                {
                    contextmenu_useronclick = "<div class=\"channel-list-context-menu\" style=\"top: " + clientY.toString() + "px; left:" + clientX.toString() + "px; \">\n\
                                                <div class='channel-list-context-menu-background'>\n\
                                                </div>\n\
                                                <div class='channel-list-context-menu-items'>\n\
                                                    <p class='context-menu-item' data-action='11'>call</p>\n\
                                                    <p class='context-menu-item' data-action='8'>poke</p>\n\
                                                    "+ string_to_append +"\n\
                                                </div>\n\
                                            </div>";
                }
                else if (is_this_music_bot_client == true)
                {
                    contextmenu_useronclick = "<div class=\"channel-list-context-menu\" style=\"top: " + clientY.toString() + "px; left:" + clientX.toString() + "px; \">\n\
                                                <div class='channel-list-context-menu-background'>\n\
                                                </div>\n\
                                                <div class='channel-list-context-menu-items'>\n\
                                                    <p class='context-menu-item' data-action='15'>manage</p>\n\
                                                    <p class='context-menu-item' data-action='5'>" + ((the_client.is_muted_by_local_client == true ) ? 'un-mute' : 'mute') + "</p>\n\
                                                    <p class='context-menu-item' data-action='18'>adjust volume</p>\n\
                                                    <p class='context-menu-item' data-action='16'>delete</p>\n\
                                                    <p class='context-menu-item' data-action='17'>set name</p>\n\
                                                    "+ string_to_append +"\n\
                                                </div>\n\
                                            </div>";
                }
                else
                {
                    contextmenu_useronclick = "<div class=\"channel-list-context-menu\" style=\"top: " + clientY.toString() + "px; left:" + clientX.toString() + "px; \">\n\
                                                <div class='channel-list-context-menu-background'>\n\
                                                </div>\n\
                                                <div class='channel-list-context-menu-items'>\n\
                                                    <p class='context-menu-item' data-action='4'>" + ((the_client.is_ignored_by_local_client == true ) ? 'un-ignore' : 'ignore') + "</p>\n\
                                                    <p class='context-menu-item' data-action='5'>" + ((the_client.is_muted_by_local_client == true ) ? 'un-mute' : 'mute') + "</p>\n\
                                                    <p class='context-menu-item' data-action='18'>adjust volume</p>\n\
                                                    <p class='context-menu-item"+add_tag_html_class+"' data-action='6'>add tag</p>\n\
                                                    <p class='context-menu-item' data-action='7'>info</p>\n\
                                                    <p class='context-menu-item' data-action='8'>poke</p>\n\
                                                    <p class='context-menu-item"+set_alias_html_class+"' data-action='19'>register</p>\n\
                                                    "+ string_to_append +"\n\
                                                </div>\n\
                                            </div>";
                }

                // <p class='context-menu-item " + ((is_local_client_admin() == false) ? 'context-menu-item-disabled' : '') + "' data-action='15'>kick</p>\n\
                // <p class='context-menu-item " + ((is_local_client_admin() == false) ? 'context-menu-item-disabled' : '') + "' data-action='16'>ban</p>\n\

            }
            else
            {
                let avatar_menu_html = "";
                if (g_avatars_allowed == true)
                {
                    avatar_menu_html = "<label class='context-menu-item' data-action='10' id='custom-file-upload-avatar' for='choose_avatar_input'>set avatar<input id='choose_avatar_input' type='file' accept='.png,.jpg,.jpeg' style='display: none'></label><p class='context-menu-item' data-action='17'>delete avatar</p>";
                }

                contextmenu_useronclick = "<div class=\"channel-list-context-menu\" style=\"top: " + clientY.toString() + "px; left:" + clientX.toString() + "px; \">\n\
                                                <div class='channel-list-context-menu-background'>\n\
                                                </div>\n\
                                                <div class='channel-list-context-menu-items'>\n\
                                                    <p class='context-menu-item' data-action='9'>set username</p>\n\
                                                    " + avatar_menu_html + "\n\
                                                    <p class='context-menu-item"+add_tag_html_class+"' data-action='6'>add tag</p>\n\
                                                    <p class='context-menu-item"+set_alias_html_class+"' data-action='19'>register</p>\n\
                                                    <p class='context-menu-item' data-action='7'>info</p>\n\
                                                </div>\n\
                                            </div>";
            }

            document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", contextmenu_useronclick);
            let choose_avatar_input = document.getElementById("choose_avatar_input");
            if (choose_avatar_input != null)
            {
                choose_avatar_input.onchange = UI.choose_avatar_input_onchange;
            }
            document.getElementById("contextmenus-container").getElementsByClassName("channel-list-context-menu")[0].style.top = clientY.toString();
            document.getElementById("contextmenus-container").getElementsByClassName("channel-list-context-menu")[0].style.left = clientX.toString();

            let event_x = clientX + 130;
            let event_y = clientY + 40;

            let contextmenu_tags = "<div class=\"add-tag-context-menu\" style=\"top: " + event_y.toString() + "px; left:" + event_x.toString() + "px; \">\n\
                                        <div class='add-tag-context-menu-background'>\n\
                                        </div>\n\
                                        <div class='add-tag-context-menu-items'>\n\
                                        </div>\n\
                                    </div>";

            document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", contextmenu_tags);

            for (let i = 0; i < g_tags.length; i++)
            {
                // guarded like every other icon lookup, otherwise the add-tag menu shows icon id 0
                // beside g_tags that were never given an icon
                let icon = (g_tags[i].has_icon == true) ? get_icon_by_icon_id(g_tags[i].tag_linked_icon_id) : null;
                let base64_icon = "";
                if (icon != null)
                {
                    base64_icon = icon.base64_icon;
                }
                let is_tag_active_for_client = get_client_by_client_id(selected_client_id).tag_ids.includes(g_tags[i].tag_id);
                let char_to_append = "☐";
                let html_class_to_append = "";
                if (is_tag_active_for_client)
                {
                    html_class_to_append = "context-menu-item-tag-active";
                    char_to_append = "☒";
                }
                let html_to_append = "<p class='add-tag-context-menu-item"+add_tag_html_class+" "+html_class_to_append+"' data-tag-id='"+g_tags[i].tag_id+"' style=\"background-image: url('"+base64_icon+"');\">"+char_to_append+" "+g_tags[i].tag_name+"</p>"
                document.getElementsByClassName("add-tag-context-menu-items")[0].insertAdjacentHTML("beforeend", html_to_append);
            }

            let contextmenu_item_count = document.getElementsByClassName("context-menu-item").length;

            for (let i = 0; i < contextmenu_item_count; i++)
            {
                let element = document.getElementsByClassName("context-menu-item")[i];
                if (parseInt(element.getAttribute("data-action")) == 10)
                {
                    continue;
                }
                document.getElementsByClassName("context-menu-item")[i].addEventListener("click", UI.contextmenuitem_onclick);

            }

            let addtagcontextmenuitem_count = document.getElementsByClassName("add-tag-context-menu-item").length;

            for (let i = 0; i < addtagcontextmenuitem_count; i++)
            {
                document.getElementsByClassName("add-tag-context-menu-item")[i].onclick = UI.add_tag_contextmenu_onclick;
            }

            let element = document.querySelector('.context-menu-item[data-action="6"]');

            if (element != null)
            {
                element.addEventListener("mouseenter", UI.add_tag_onmouseenter);
                element.addEventListener("mouseleave", UI.add_tag_onmouseleave);
            }
        }
    },

    // font item picked in the chat-input context menu: stores it in g_selected_font and
    // swaps the input's css class so typing previews that font
    contextmenuitem_chat_input_container_onclick: function(event)
    {
        let action = event.srcElement.getAttribute("data-action");

        g_selected_font = action;
        UI.delete_contextmenus();
        console.log(g_selected_font);

        // this is the correct way to clear classList

        document.getElementById("chat-input-container-text-input").setAttribute("class", "")
        document.getElementById("chat-input-container-text-input").classList.add(action);
    },

    // contextmenu handler
    contextmenuitem_onclick: function(event)
    {
        event.stopPropagation();

        let action = event.srcElement.getAttribute("data-action");

        UI.delete_contextmenus();

        if (action == 0)
        {
            document.getElementById("background-container").style.display = "block";
            document.getElementById("channel-properties-edit-container").style.display = "block";
            document.getElementById("create-channel-hidden-parent-id").value = selected_channel_id;
            document.getElementById("create-channel-button").style.display = "";
            document.getElementById("edit-channel-button").style.display = "none";

            // one dialog serves both jobs, so say which one this is
            document.querySelector("#channel-properties-edit .dialog-title").innerHTML = "Create Channel";

            document.getElementById("channel-properties-input-channel-name").value = "";
            document.getElementById("channel-properties-input-channel-description").value = "";
            document.getElementById("channel-properties-input-channel-password").value = "";
            document.getElementById("channel-properties-disable-audio-checkbox").checked = false;
            document.getElementById("channel-properties-limit-clients-checkbox").checked = false;
            document.getElementById("channel-properties-input-max-clients").value = "";
            UI.refresh_channel_limit_input_visibility();

            // creating a new channel: no channel exists yet, so the icon can only be set later via edit
            g_channel_properties_edit_channel_id = null;
            document.getElementById("channel-properties-icon-label-row").style.display = "none";
            document.getElementById("channel-properties-icon-row").style.display = "none";
        }
        else if (action == 1)
        {
            let message_object = {
                message: {
                    type: "delete_channel_request",
                    channel_id: parseInt(selected_channel_id)
                }
            };

            console.log(message_object);

            send_message_object(message_object);
            console.log("delete channel requested: channel_id=" + selected_channel_id);

            document.getElementById("current-channel-description").innerHTML = "";
        }
        else if (action == 2)
        {
            document.getElementById("background-container").style.display = "block";
            document.getElementById("channel-properties-edit-container").style.display = "block";
            document.getElementById("create-channel-hidden-parent-id").value = selected_channel_id;

            let index = get_channel_index_in_array_by_channel_id(g_channel_list, selected_channel_id);
            // assign the raw values: input.value is a property assignment, not an HTML sink, so it
            // cannot execute markup, and escaping here would show entities (e.g. an apostrophe as
            // &#039;) in the edit box and corrupt the value when the dialog is saved back
            document.getElementById("channel-properties-input-channel-name").value = g_channel_list[index].name;
            document.getElementById("channel-properties-input-channel-description").value = g_channel_list[index].description;
            document.getElementById("channel-properties-disable-audio-checkbox").checked = g_channel_list[index].is_audio_enabled == false;
            document.getElementById("channel-properties-limit-clients-checkbox").checked = g_channel_list[index].is_client_limit_active == true;
            document.getElementById("channel-properties-input-max-clients").value = (g_channel_list[index].is_client_limit_active == true && g_channel_list[index].max_client_count > 0) ? g_channel_list[index].max_client_count : "";
            UI.refresh_channel_limit_input_visibility();

            document.getElementById("create-channel-button").style.display = "none";
            document.getElementById("edit-channel-button").style.display = "";

            // same dialog as channel creation, so put its own name back
            document.querySelector("#channel-properties-edit .dialog-title").innerHTML = "Edit Channel";

            // editing an existing channel: expose the channel-icon row and paint the current icon.
            // the row shows for everyone (clicking the box opens the shared icon picker); the
            // server decides whether a set_channel_icon request is actually allowed
            g_channel_properties_edit_channel_id = parseInt(selected_channel_id);
            document.getElementById("channel-properties-icon-label-row").style.display = "";
            document.getElementById("channel-properties-icon-row").style.display = "";
            UI.refresh_channel_edit_icon_box(g_channel_list[index]);
        }
        else if (action == 3)
        {
            if (selected_channel_id == current_channel_id)
            {
                return;
            }

            let selected_channel = get_channel_by_id(g_channel_list, selected_channel_id);

            if (selected_channel.is_using_password == true)
            {
                document.getElementById("channel-password-enter-container").style.display = "block";
                document.getElementById("background-container").style.display = "block";
            }
            else
            {
                let message_object = {
                    message: {
                        type: "join_channel_request",
                        channel_id: parseInt(selected_channel_id),
                        channel_password: ""
                    }
                };

                send_message_object(message_object);
            }
        }
        else if (action == 4) // ignore client locally
        {
            let client_id_of_interest = parseInt(selected_client_id);
            let client_index = get_client_index_in_array_by_client_id(client_id_of_interest);

            if (client_index == -1 || client_id_of_interest == local_client_id)
            {
                return; // not found
            }

            g_client_list[client_index].is_ignored_by_local_client = !g_client_list[client_index].is_ignored_by_local_client;
            console.log("local ignore toggled: client_id=" + client_id_of_interest + " ignored=" + g_client_list[client_index].is_ignored_by_local_client);

            if (g_client_list[client_index].is_ignored_by_local_client == true)
            {
                let element = document.querySelector('.connected-client[data-connected-client-id="'+selected_client_id+'"]').getElementsByClassName('client-ignore-state')[0].style.display = "block";
            }
            else
            {
                let element = document.querySelector('.connected-client[data-connected-client-id="'+selected_client_id+'"]').getElementsByClassName('client-ignore-state')[0].style.display = "none";
            }

        }
        else if (action == 5) // mute client locally
        {
            let client_id_of_interest = parseInt(selected_client_id);
            let client_index = get_client_index_in_array_by_client_id(client_id_of_interest);

            if (client_index == -1 || client_id_of_interest == local_client_id)
            {
                return; // not found
            }

            g_client_list[client_index].is_muted_by_local_client = !g_client_list[client_index].is_muted_by_local_client;
            console.log("local mute toggled: client_id=" + client_id_of_interest + " muted=" + g_client_list[client_index].is_muted_by_local_client);

            if (g_client_list[client_index].is_muted_by_local_client == true)
            {
                let element = document.querySelector('.connected-client[data-connected-client-id="'+selected_client_id+'"]').getElementsByClassName('client-mute-state')[0].style.display = "block";
            }
            else
            {
                let element = document.querySelector('.connected-client[data-connected-client-id="'+selected_client_id+'"]').getElementsByClassName('client-mute-state')[0].style.display = "none";
            }

        }
        else if (action == 7) // info: everyone sees the safe facts (name, alias, g_tags, last seen); admins additionally get ip/country/identity from the server
        {
            UI.show_client_info_popup(selected_client_id);

            if (is_local_client_admin() == false)
            {
                return;
            }

            let message_object = {
                message: {
                    type: "get_client_info",
                    client_id: parseInt(selected_client_id),
                }
            };

            send_message_object(message_object);
        }
        else if (action == 8)
        {
            document.getElementById("poke-client-enter-container").style.display = "block";
        }
        else if (action == 19)
        {
            // prefill with the current alias so the admin edits rather than retypes; empty send clears it
            let the_client = get_client_by_client_id(selected_client_id);
            document.getElementById("input-set-alias").value = (the_client != null && the_client.alias != null) ? the_client.alias : "";
            document.getElementById("set-alias-enter-container").style.display = "block";
        }
        else if (action == 18) // adjust this client's playback volume (local only)
        {
            let current_gain = g_client_volume_by_id[selected_client_id];
            if (current_gain == null) { current_gain = 1.0; }
            let percent = Math.round(current_gain * 100);

            // remember which client the slider targets on the slider itself, so it can't drift if
            // selected_client_id changes while the dialog is open
            document.getElementById("adjust-volume-slider").setAttribute("data-target-client-id", selected_client_id);
            document.getElementById("adjust-volume-slider").value = percent;
            document.getElementById("adjust-volume-value-label").innerHTML = percent + "%";
            document.getElementById("adjust-volume-username-label").innerHTML = sanitize_string(get_username_by_client_id(selected_client_id));
            document.getElementById("adjust-volume-container").style.display = "block";
        }
        else if (action == 9)
        {
            // a registered user's name is admin-controlled (set via "register username"),
            // so he cannot rename himself out of it - the server denies it too
            let self_client = get_client_by_client_id(local_client_id);
            if (self_client != null && typeof self_client.alias === "string" && self_client.alias.length > 0)
            {
                custom_alert("your username is registered - only an admin can change it");
                return;
            }
            document.getElementById("input-set-new-username").value = self_client.username;
            document.getElementById("set-new-username-enter-container").style.display = "block";
        }
        else if (action == 17) // delete your own avatar
        {
            if (g_avatars_allowed == false) { return; }

            let message_object = { message: { type: "delete_avatar" } };
            send_message_object(message_object);

            // clear our own avatar in the ui immediately; the server's avatar_changed confirms
            apply_avatar_to_ui(local_client_id, "");
        }
        else if (action == 11)
        {
            // play sound

            if (g_are_sound_effects_enabled)
            {
                g_sound_effects.call.play();
            }

            let element = document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + selected_client_id + '"]');
            if (element != null)
            {
                element.style.display = "inline-block";
                document.getElementById("marquee-song-name-client-id-" + selected_client_id).innerHTML = "calling ... ...";
            }

            setTimeout( () => {
                if (document.getElementById("marquee-song-name-client-id-" + selected_client_id) != null) {
                    document.getElementById("marquee-song-name-client-id-" + selected_client_id).innerHTML = "";
                }
                document.querySelector('.marquee-music-playing-container[data-marquee-music-playing-container-id="' + selected_client_id + '"]').style.display = "none";
            }, 14000);

            // after 17 seconds hide it..
            // if user accepts call, the marquee-music-playing-container gets deleted because he is switched to different channel and the html itself gets deleted
            // so no need to take care of html in that case

            let message_object = {
                message: {
                    type: "call_idle_client_request",
                    client_id: parseInt(selected_client_id),
                }
            };

            send_message_object(message_object);
        }
        else if (action == 12)
        {
            let message_object = {
                message: {
                    type: "kick",
                    client_id: parseInt(selected_client_id),
                }
            };

            send_message_object(message_object);
            console.log("kick requested: client_id=" + selected_client_id);
        }
        else if (action == 13)
        {
            let message_object = {
                message: {
                    type: "ban",
                    client_id: parseInt(selected_client_id),
                }
            };

            send_message_object(message_object);
            console.log("ban requested: client_id=" + selected_client_id);
        }
        else if (action == 14)
        {
            let message_object = {
                message: {
                    type: "create_music_bot",
                    channel_id: parseInt(selected_channel_id),
                    music_bot_username: "radio2198124"
                }
            };

            send_message_object(message_object);
        }
        else if (action == 15)
        {
            client_msg.send_musicbot_song_list_request();
        }
        else if (action == 16)
        {

            let message_object = {
                message: {
                    type: "delete_music_bot",
                    channel_id: get_client_by_client_id(selected_client_id).channel_id,
                }
            };

            send_message_object(message_object);
        }
        else if (action == 17)
        {
            document.getElementById("input-set-new-username").value = get_client_by_client_id(selected_client_id).username;
            document.getElementById("set-new-username-enter-container").style.display = "block";
        }
    },

    // chat-message menu item: action 0 asks the server to delete the message, action 1 makes
    // the local message contenteditable for in-place editing (saved on blur)
    chat_message_contextmenuitem_onclick: function(event)
    {
        event.stopPropagation();
        let action = event.target.getAttribute("data-action");

        UI.delete_chat_message_contextmenu();

        if (action == 0)
        {
            let message_object = {
                message: {
                    type: "delete_chat_message_request",
                    message_id: g_selected_server_chat_message_id,
                    receiver_type: g_chat_message_receiver_type,
                    receiver_id: parseInt(chat_message_receiver_id)
                }
            };

            send_message_object(message_object);
        }
        else if (action == 1)
        {
            let element = document.querySelector('.local-single-chat-message-content-p[data-single-chat-message-server-message-id="' + g_selected_server_chat_message_id + '"]');
            if (element != null)
            {
                element.setAttribute("contenteditable", "true");

                // forget the logic, just use setTimeout
                window.setTimeout(function ()
                {
                    element.focus();
                }, 0);

                element.addEventListener("blur", local_chat_message_onblur);
            }
        }
    },

    // right click on an own chat message: remembers its server message id in
    // g_selected_server_chat_message_id and opens the delete/edit context menu at the cursor
    single_chat_message_onrightclick: function(event)
    {
        event.stopPropagation();

        if (event.which == 3)
        {

            let elements_count1 = document.getElementsByClassName("local-single-chat-message-content-p").length;

            for (let i = 0; i < elements_count1; i++)
            {
                document.getElementsByClassName("local-single-chat-message-content-p")[i].classList.remove("single-chat-message-content-p-hover");
            }

            event.target.classList.add("single-chat-message-content-p-hover");

            // delete old contextmenus

            let elements_count = document.getElementsByClassName("chat-message-context-menu").length;

            for (let i = 0; i < elements_count; i++)
            {
                document.getElementsByClassName("chat-message-context-menu")[0].remove();
            }

            // append new contextmenus

            g_selected_server_chat_message_id = parseInt(event.target.getAttribute("data-single-chat-message-server-message-id"));

            let contextmenu_html = "<div class=\"chat-message-context-menu\" style=\"top: " + event.clientY.toString() + "px; left:" + event.clientX.toString() + "px; \">\n\
                                                <div class='chat-message-context-menu-background'>\n\
                                                </div>\n\
                                                <div class='chat-message-context-menu-items'>\n\
                                                    <p class='chat-message-context-menu-item' data-action='0'>delete</p>\n\
                                                    <p class='chat-message-context-menu-item' data-action='1'>edit</p>\n\
                                                </div>\n\
                                            </div>";

            document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", contextmenu_html);
            document.getElementById("contextmenus-container").getElementsByClassName("chat-message-context-menu")[0].style.top = event.clientY.toString();
            document.getElementById("contextmenus-container").getElementsByClassName("chat-message-context-menu")[0].style.left = event.clientX.toString();

            let contextmenu_item_count = document.getElementsByClassName("chat-message-context-menu-item").length;

            for (let i = 0; i < contextmenu_item_count; i++)
            {
                document.getElementsByClassName("chat-message-context-menu-item")[i].addEventListener("mousedown", UI.chat_message_contextmenuitem_onclick);
            }
        }

    },

    // send button: sends the message and collapses the image-upload preview
    send_chat_input_on_click: function()
    {
        send_chat_message();
        document.getElementById("image-upload-preview").style.display = "none";

        // a refused file stays attached, so its chip comes back
        render_pending_chat_file_chip();
    },

    // connect button on the login form: hands the driver a target
    connect_button_onclick: function()
    {
        submit_connection_target_from_ui();
    },

    // enter in the chat input sends the message
    chat_input_on_keyup: function()
    {
        if (event.key == "Enter" || event.keyCode == 13)
        {
            send_chat_message();
            return;
        }

        // everything else counts as writing; the send itself is throttled inside
        send_typing_indicator();
    },

    // image picked for the chat: the preview path lives in attach_chat_picture (chat-files.js),
    // shared with drag & drop, which also explains a refusal over 4mb
    choose_image_input: function()
    {
        if (typeof window === 'undefined')   // multiple webworkers are used within same file
        {
            return;
        }

        let files = document.getElementById('choose_image_input').files;

        if (files.length > 0)
        {
            attach_chat_picture(files[0]);
        }
    },

    // "add key" on the connect form: appends a "key N" label row and a value input (with its
    // hover remove button) to the verification lists and bumps g_custom_key_count
    add_key_button_on_click: function()
    {
        const node = document.createElement("div");
        node.style.paddingTop = "10px";
        node.style.padingBottom = "10px";
        node.className = "text-input-container";
        node.setAttribute("data-key-id", g_custom_key_count);

        const node2 = document.createElement("input");
        node2.className = "text-input-pretty";
        node2.value = "key " + g_custom_key_count;
        node2.disabled = true;
        node.appendChild(node2);

        document.getElementById('connect-form-sub-container-1').appendChild(node);

        let generated_id_for_html_input = "input-key-" + g_custom_key_count;

        const node3 = document.createElement("div");
        node3.style.paddingTop = "10px";
        node3.style.padingBottom = "10px";
        node3.className = "text-input-container";
        node3.setAttribute("data-key-id", g_custom_key_count);

        const node4 = document.createElement("input");
        node4.className = "text-input-pretty";
        node4.id = generated_id_for_html_input;
        node4.setAttribute("data-id", g_custom_key_count);

        node3.appendChild(node4);

        // per-key remove button (hover-revealed; styled in flags.style so every theme gets
        // it). removes both the label and value row for this key. connect-time collection
        // iterates the surviving key inputs, so a gap left behind is harmless.
        const remove_button = document.createElement("div");
        remove_button.className = "remove-key-button";
        remove_button.textContent = "✕";
        remove_button.title = "remove this key";
        const removed_key_id = g_custom_key_count;
        remove_button.onclick = function()
        {
            const label_node = document.querySelector('#connect-form-sub-container-1 .text-input-container[data-key-id="' + removed_key_id + '"]');
            const value_node = document.querySelector('#connect-form-sub-container-2 .text-input-container[data-key-id="' + removed_key_id + '"]');
            if (label_node != null) { label_node.remove(); }
            if (value_node != null) { value_node.remove(); }

            // renumber the visible "key N" labels so they stay 0..n-1 with no gaps
            // (the value inputs keep their unique ids; collection is by dom order)
            const remaining_labels = document.querySelectorAll('#connect-form-sub-container-1 .text-input-container[data-key-id] .text-input-pretty');
            for (let j = 0; j < remaining_labels.length; j++)
            {
                remaining_labels[j].value = "key " + j;
            }
        };
        node3.appendChild(remove_button);

        document.getElementById('connect-form-sub-container-2').appendChild(node3);
        document.getElementById("verification-system").scrollTop = document.getElementById("verification-system").scrollHeight;
        g_custom_key_count++;
    },

    // toggles the debug log textarea between visible and hidden
    show_hide_log_on_click: function()
    {
        if (g_textarea_log.style.display == "none")
        {
            g_textarea_log.style.display = "block";
        }
        else if (g_textarea_log.style.display == "block")
        {
            g_textarea_log.style.display = "none";
        }
    },

    // leaving the inline username input: stores the new name in g_client_list/g_local_username
    // and sends the rename request to the server (an empty input is ignored)
    connected_local_user_input_on_focusout: function()
    {
        let new_username = this.value;

        if (new_username.length == 0)
        {
            return;
        }

        // a registered user's name is admin-controlled; the server rejects a self-rename,
        // so do not send one, say why and snap the field back to the registered name
        let self_client = get_client_by_client_id(local_client_id);
        if (self_client != null && typeof self_client.alias === "string" && self_client.alias.length > 0)
        {
            this.value = self_client.username;
            custom_alert("your username is registered - only an admin can change it");
            return;
        }

        let index = get_client_index_in_array_by_client_id(local_client_id);

        g_client_list[index].username = new_username;
        g_local_username = new_username;
        { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.setAttribute('value', g_local_username); } }

        client_msg.send_change_client_username_request(new_username, local_client_id);
        console.log("local username changed");
    },

    // closes the import-identity dialog and its background overlay
    close_button_identity_string_use: function(event)
    {
        document.getElementById("identity-string-use-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
    },

    // mousedown on a channel row: left click shows the channel description in the right pane,
    // right click (or touch) highlights the row and opens the create/delete/edit/join menu
    single_channel_onmousedown: function(event, is_touch_event = null, touch_clientX = null, touch_clientY = null, touch_current_target = null)
    {
        if (event && typeof event.stopPropagation === "function")
        {
            event.stopPropagation();
        }

        let clientX = 0;
        let clientY = 0;
        let currentTarget = null;

        if (event.currentTarget == null)
        {
            currentTarget = touch_current_target;
        }
        else
        {
            currentTarget = event.currentTarget;
        }

        if (currentTarget.classList.contains("idle-channel"))
        {
            return;
        }

        if (is_touch_event == null)
        {
            clientX = event.clientX;
            clientY = event.clientY;
        }
        else
        {
            clientX = touch_clientX;
            clientY = touch_clientY;
        }

        if (event.which == 1)
        {
            let channel_id = parseInt(currentTarget.getAttribute("data-channel-id"));
            let index = get_channel_index_in_array_by_channel_id(g_channel_list, channel_id);

            document.getElementById("current-client-description").style.display = "none";
            document.getElementById("current-channel-description").style.display = "block";

            document.getElementById("current-channel-description").innerHTML = sanitize_string(g_channel_list[index].description);
        }
        if (event.which == 3 || is_touch_event == true)
        {
            let elements = document.getElementsByClassName('single-channel');

            for (let i = 0; i < elements.length; i++)
            {
                elements[i].style.backgroundColor = "";
            }

            let elements1 = document.getElementsByClassName('connected-client');

            for (let i = 0; i < elements1.length; i++)
            {
                elements1[i].style.backgroundColor = "";
            }

            currentTarget.style.backgroundColor = "var(--row-rightclick-selection, #333030)";
            selected_channel_id = parseInt(currentTarget.getAttribute("data-channel-id"));

            UI.delete_contextmenus();

            // the root channel can be renamed and edited, but never deleted - the
            // server would have nowhere to put the clients standing in it
            let selected_channel_index = get_channel_index_in_array_by_channel_id(g_channel_list, selected_channel_id);
            let delete_menu_item_html =
                (selected_channel_index != -1 && g_channel_list[selected_channel_index].is_root_channel == true)
                    ? "" : "<p class='context-menu-item' data-action='1'>delete</p>";

            let contextmenu_html = "<div class=\"channel-list-context-menu\" style=\"top: " + clientY + "px; left:" + clientX.toString() + "px; \">\n\
                                        <div class='channel-list-context-menu-background'>\n\
                                        </div>\n\
                                        <div class='channel-list-context-menu-items'>\n\
                                            <p class='context-menu-item' data-action='0'>create</p>\n\
                                            " + delete_menu_item_html + "\n\
                                            <p class='context-menu-item' data-action='2'>edit</p>\n\
                                            <p class='context-menu-item' data-action='3'>join</p>\n\
                                            <p class='context-menu-item' data-action='14'>create music bot</p>\n\
                                        </div>\n\
                                    </div>";

            document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", contextmenu_html);
            document.getElementById("contextmenus-container").getElementsByClassName("channel-list-context-menu")[0].style.top = clientY.toString();
            document.getElementById("contextmenus-container").getElementsByClassName("channel-list-context-menu")[0].style.left = clientX.toString();

            let contextmenu_item_count = document.getElementsByClassName("context-menu-item").length;

            for (let i = 0; i < contextmenu_item_count; i++)
            {
                document.getElementsByClassName("context-menu-item")[i].addEventListener("click", UI.contextmenuitem_onclick);
            }

            let channel_id = parseInt(currentTarget.getAttribute("data-channel-id"));
            let index = get_channel_index_in_array_by_channel_id(g_channel_list, channel_id);

            document.getElementById("current-channel-description").innerHTML = sanitize_string(g_channel_list[index].description);
        }
    },

    // fills the shared client-info popup with what THIS client already knows about a person:
    // display name, the raw username behind an alias, g_tags and (when the server records it)
    // when a registered person was last connected. the admin-only rows the server answers
    // with (ip, country, identity, connected time) are emptied here and filled in later by
    // process_client_info_from_server when that reply arrives, so a non-admin never sees
    // stale values from a previous look. same popup every theme uses.
    show_client_info_popup: function(client_id)
    {
        let client = get_client_by_client_id(client_id);
        if (client == null) { return; }

        let alias_text = (typeof client.alias === "string") ? client.alias : "";

        UI.show_client_info_popup_shell();

        // "user" is the name he chose, "alias" the one an admin registered on his identity
        document.getElementById("client-info-username").innerText = client.username;

        document.getElementById("client-info-alias").innerText = (alias_text.length > 0) ? alias_text : "-";

        let tag_names = [];
        for (let i = 0; i < client.tag_ids.length; i++)
        {
            let tag = get_tag_by_tag_id(client.tag_ids[i]);
            if (tag != null) { tag_names.push(tag.tag_name); }
        }
        document.getElementById("client-info-tags").innerText = (tag_names.length > 0) ? tag_names.join(", ") : "-";

        // this person is connected right now; the stored last-seen only matters once they leave
        // connected right now, so the answer to "when were they last seen" is: now
        let live_status = (client.is_idle == true) ? "idle" : "online";
        document.getElementById("client-info-status").innerText = live_status;
        document.getElementById("client-info-status").className = "client-info-status-" + live_status;
        document.getElementById("client-info-last-seen").innerText = "online now";

        // admin rows: cleared now, filled by the server reply if we are an admin
        document.getElementById("client-info-connected").innerText = "";
        document.getElementById("client-info-ip").innerText = "";
        document.getElementById("client-info-country").innerText = "";
        document.getElementById("client-info-last-action").innerText = "";
        document.getElementById("client-info-identity").innerText = "";

        document.getElementById("client-info-container").style.display = "block";
    },

    // a person we have an offline conversation with just connected: merge that context into
    // their live private chat so the thread continues in one place. called on connect, and
    // as a safety net right before sending.
    // the RENDER half only. promote_offline_chat_context_state (main.js) has already decided
    // what happens and mutated g_chat_context_array; this moves the markup to match it.
    // the decisions are NOT re-derived from the dom here - asking twice risks two answers.
    promote_offline_chat_context_render: function(promotion, client)
    {
        if (promotion == null || promotion.did_promote == false) { return; }
        if (client == null) { return; }

        let offline_context = document.getElementById(promotion.offline_context_id);
        let live_context = document.getElementById(promotion.live_context_id);

        if (promotion.had_live_context == false)
        {
            // no live chat yet: the offline one simply becomes it, history and all
            if (offline_context != null)
            {
                offline_context.id = promotion.live_context_id;
            }
        }
        else if (offline_context != null && live_context != null)
        {
            // both exist: move what was said while they were away into the live thread
            while (offline_context.firstChild != null)
            {
                live_context.appendChild(offline_context.firstChild);
            }

            offline_context.remove();
        }

        // the pill (desktop themes) was keyed by alias; re-key it to the live client
        let offline_selector = document.querySelector('[data-chat-context-selector-id="offline-' + client.alias + '"]');
        if (offline_selector != null)
        {
            if (document.querySelector('[data-chat-context-selector-id="user-' + client.client_id + '"]') != null)
            {
                offline_selector.remove();
            }
            else
            {
                offline_selector.setAttribute("data-chat-context-selector-id", "user-" + client.client_id);
                let remove_button = offline_selector.querySelector(".remove-chat-context-selector");
                if (remove_button != null) { remove_button.setAttribute("data-chat-context-remove-selector-id", "user-" + client.client_id); }
            }
        }

        // the receiver globals were already switched over by the state half; this only keeps
        // the promoted thread on screen
        if (promotion.was_open == true)
        {
            let live_element = document.getElementById(promotion.live_context_id);

            if (live_element != null)
            {
                live_element.style.display = "block";
            }

            UI.enable_inputs();
        }

        UI.schedule_member_list_sync();
    },

    // opens (or re-opens) the conversation with somebody who is offline. the input bar then
    // addresses them by alias; send_chat_message routes it to the offline path.
    open_offline_chat_context: function(offline_contact)
    {
        let context_id = "chat-context-offline-" + offline_contact.alias;

        let can_be_delivered = (typeof offline_contact.public_key === "string" && offline_contact.public_key.length > 0);

        if (document.getElementById(context_id) == null)
        {
            // without their public key nothing can be encrypted for them, so say so here
            // instead of letting somebody type a message that goes nowhere
            let header_note = can_be_delivered
                ? " (offline - they get this when they return)"
                : " (offline - this server does not hold messages for people who are away)";

            let html_to_append = "<div class=\"chat-context\" id=\"" + context_id + "\">\n\
                                    <div class=\"single-server-message\">now talking to user: " + sanitize_string(offline_contact.alias) + header_note + "</div>\n\
                                </div>";
            document.getElementById('chat-context-container').insertAdjacentHTML("beforeend", html_to_append);

            g_chat_context_array.push({
                type: "user",
                chat_context_id: context_id,
                last_known_message_sender_username: ""
            });
        }

        for (let i = 0; i < document.getElementsByClassName("chat-context").length; i++)
        {
            document.getElementsByClassName("chat-context")[i].style.display = "none";
        }

        document.getElementById(context_id).style.display = "block";

        g_current_chat_context_id = context_id;
        g_chat_message_receiver_type = "user";
        chat_message_receiver_id = -1;              // nobody is connected under this alias
        g_offline_chat_recipient_alias = offline_contact.alias;

        UI.enable_inputs();
        UI.schedule_member_list_sync();

        document.getElementById('chat-context-container').scrollTop = document.getElementById('chat-context-container').scrollHeight;
    },

    // the same popup for somebody who is registered here but not connected: everything we
    // know comes from the stored-clients snapshot (alias, g_tags and, when the server records
    // it, when they were last here). no live client, so no admin rows at all.
    show_offline_contact_info_popup: function(alias_text)
    {
        let stored_client = null;
        for (let i = 0; i < g_offline_client_list.length; i++)
        {
            if (g_offline_client_list[i].alias.toLowerCase() == ("" + alias_text).toLowerCase())
            {
                stored_client = g_offline_client_list[i];
                break;
            }
        }

        if (stored_client == null) { return; }

        UI.show_client_info_popup_shell();

        // nobody is connected under this identity, so there is no live username to show -
        // an empty row hides itself, leaving alias / g_tags / last seen
        document.getElementById("client-info-username").innerText = "";
        document.getElementById("client-info-alias").innerText = stored_client.alias;

        let tag_names = [];
        for (let i = 0; i < stored_client.tag_ids.length; i++)
        {
            let tag = get_tag_by_tag_id(stored_client.tag_ids[i]);
            if (tag != null) { tag_names.push(tag.tag_name); }
        }
        document.getElementById("client-info-tags").innerText = (tag_names.length > 0) ? tag_names.join(", ") : "-";

        // 0 means the server does not record last-seen (or never saw this identity leave)
        // nobody is connected under this identity. "last seen" is a TIME or nothing at
        // all - the offline state itself belongs in the status row above. a zero stamp
        // means the server does not record last-seen (or never saw this identity leave)
        document.getElementById("client-info-status").innerText = "offline";
        document.getElementById("client-info-status").className = "client-info-status-offline";
        document.getElementById("client-info-last-seen").innerText = (stored_client.last_seen > 0) ? UI.format_time_ago(stored_client.last_seen) : "no data available";

        document.getElementById("client-info-connected").innerText = "";
        document.getElementById("client-info-ip").innerText = "";
        document.getElementById("client-info-country").innerText = "";
        document.getElementById("client-info-last-action").innerText = "";
        document.getElementById("client-info-identity").innerText = "";

        document.getElementById("client-info-container").style.display = "block";
    },

    // a unix timestamp (seconds) as plain english: "just now", "5 minutes ago",
    // "2 hours ago", "3 weeks ago", "1 year ago". the exact date means nothing to a
    // normal user - how long ago does.
    format_time_ago: function(unix_seconds)
    {
        let seconds_ago = Math.floor(new Date().valueOf() / 1000) - unix_seconds;

        // clock skew between server and phone can put it slightly in the future
        if (seconds_ago < 0) { seconds_ago = 0; }

        if (seconds_ago < 60) { return "just now"; }

        // { seconds per unit, singular name } from smallest to largest; a month is the
        // average 30.44 days and a year 365.25 days, so long gaps do not drift
        let units = [
            { seconds: 60, name: "minute" },
            { seconds: 3600, name: "hour" },
            { seconds: 86400, name: "day" },
            { seconds: 604800, name: "week" },
            { seconds: 2629800, name: "month" },
            { seconds: 31557600, name: "year" }
        ];

        let chosen = units[0];
        for (let i = 0; i < units.length; i++)
        {
            if (seconds_ago >= units[i].seconds) { chosen = units[i]; }
        }

        let count = Math.floor(seconds_ago / chosen.seconds);
        return count + " " + chosen.name + ((count == 1) ? "" : "s") + " ago";
    },

    // "peter novak" -> "PN", "noobnoob" -> "NO". used as a stand-in avatar so people
    // without a picture still get a circle worth looking at
    initials_for_name: function(name)
    {
        let trimmed = ("" + name).trim();
        if (trimmed.length == 0) { return "?"; }

        let words = trimmed.split(/[\s._-]+/).filter(function(word) { return word.length > 0; });

        if (words.length >= 2)
        {
            return (words[0].charAt(0) + words[1].charAt(0)).toUpperCase();
        }

        return trimmed.substring(0, 2).toUpperCase();
    },

    // creates the extra rows the markup does not carry (username behind an alias, g_tags,
    // last seen). called by both info paths; does nothing after the first time.
    show_client_info_popup_shell: function()
    {
        if (document.getElementById("client-info-extra-rows") != null) { return; }

        let content = document.getElementById("client-info-content");
        let extra_rows = document.createElement("div");
        extra_rows.id = "client-info-extra-rows";
        extra_rows.innerHTML = "<p class='client-info-row'><span class='client-info-label'>alias</span><span id='client-info-alias'></span></p>"
                                + "<p class='client-info-row'><span class='client-info-label'>status</span><span id='client-info-status'></span></p>"
                                + "<p class='client-info-row'><span class='client-info-label'>tags</span><span id='client-info-tags'></span></p>"
                                + "<p class='client-info-row'><span class='client-info-label'>last seen</span><span id='client-info-last-seen'></span></p>";
        content.insertBefore(extra_rows, content.children[1]);
    },

    // activates a theme by unmuting its stylesheet (all others get an impossible media query),
    // remaps renamed/unknown names, rewires the member list and optionally persists the choice
    apply_theme: function(themename, persist)
    {
        for (let i = 0; i < document.getElementsByClassName("style-theme-html-element").length; i++)
        {
            document.getElementsByClassName("style-theme-html-element")[i].setAttribute("media","(max-width: 1px)");
        }

        if (themename == "light-theme")
        {
            document.getElementById("style-theme-light").removeAttribute("media");
        }
        else if (themename == "default")
        {
            document.getElementById("style-theme-default").removeAttribute("media");
        }
        else if (themename == "oldschool")
        {
            document.getElementById("style-theme-oldschool").removeAttribute("media");
        }
        else if (themename == "oldschool-variant")
        {
            document.getElementById("style-theme-oldschool-variant").removeAttribute("media");
        }
        else if (themename == "termix")
        {
            document.getElementById("style-theme-termix").removeAttribute("media");
        }
        else if (themename == "default-mobile")
        {
            document.getElementById("style-theme-default-mobile").removeAttribute("media");
        }
        else if (themename == "simpledark")
        {
            document.getElementById("style-theme-simpledark").removeAttribute("media");
        }
        else if (themename == "bluebell")
        {
            document.getElementById("style-theme-bluebell").removeAttribute("media");
        }
        else
        {
            // unknown name (e.g. a theme that was removed but still sits in localStorage):
            // fall back to the default sheet instead of leaving the page unstyled
            document.getElementById("style-theme-default").removeAttribute("media");
            themename = "default";
        }

        // the new theme may show or hide the right-pane member list; (re)wire its live mirror
        // accordingly. driven off the computed style so it stays generic - any theme that makes
        // #member-list-container visible gets the mirror, no theme name hard-coded here
        UI.refresh_member_list_state();

        // persist defaults to true (user picks); the server-baked default passes false so it never sticks in localStorage
        if (persist !== false) { try { localStorage.setItem("lemon_theme", themename); } catch (e) { console.warn("failed to persist theme selection:", e.message); } }
    },

    // the right-pane member list is a live clone of the left tree's user rows. it is only
    // maintained while a theme makes #member-list-container visible (currently termix); other
    // themes pay nothing - the observer is disconnected and the list is cleared.
    refresh_member_list_state: function()
    {
        let container = document.getElementById("member-list-container");
        if (container == null) { return; }

        let is_visible = getComputedStyle(container).display != "none";

        // the member list IS the avatar grid: when it's visible (discord-style theme) and the
        // server allows avatars, paint avatars into the circles and bulk-load everyone's.
        let grid_now_visible = (is_visible == true && g_avatars_allowed == true);
        let grid_just_appeared = (grid_now_visible == true && g_avatar_grid_visible == false);
        g_avatar_grid_visible = grid_now_visible;

        if (is_visible == true)
        {
            if (g_member_list_observer == null)
            {
                g_member_list_observer = new MutationObserver(function() { UI.schedule_member_list_sync(); });
            }

            let tree = document.getElementById("channel-list-container");
            if (tree != null)
            {
                g_member_list_observer.observe(tree, { childList: true, subtree: true, attributes: true, attributeFilter: ["class", "value"] });
            }

            // idle people live in their own container next to the tree; without watching
            // it too, an idle client disconnecting would linger in the strip
            let idle_container = document.getElementById("idle-clients-container");
            if (idle_container != null)
            {
                g_member_list_observer.observe(idle_container, { childList: true, subtree: true, attributes: true, attributeFilter: ["class", "value"] });
            }

            UI.sync_member_list();

            // switching into the discord theme after connect: pull every avatar for the grid now
            if (grid_just_appeared == true) { enqueue_all_avatars_for_loading(); }
        }
        else
        {
            if (g_member_list_observer != null)
            {
                g_member_list_observer.disconnect();
            }

            let body = document.getElementById("member-list-body");
            if (body != null) { body.innerHTML = ""; }
            let header = document.getElementById("member-list-header");
            if (header != null) { header.textContent = ""; }
        }
    },

    // coalesce a burst of tree mutations (adding many rows, a user toggling its speaking class)
    // into at most one rebuild per animation frame
    schedule_member_list_sync: function()
    {
        if (g_member_list_sync_scheduled == true) { return; }
        g_member_list_sync_scheduled = true;
        requestAnimationFrame(function()
        {
            g_member_list_sync_scheduled = false;
            UI.sync_member_list();
        });
    },

    // rebuild the member list from the current tree rows. clones are stripped of their ids
    // (duplicate ids would break getElementById on the real rows) and the local client's
    // username <input> is rendered as plain text; the list is display-only.
    sync_member_list: function()
    {
        let body = document.getElementById("member-list-body");
        let header = document.getElementById("member-list-header");
        if (body == null) { console.warn("sync_member_list: member-list-body missing; skipping"); return; }

        let tree = document.getElementById("channel-list-container");
        // idle people are MOVED out of the tree into #idle-clients-container, but they
        // are still connected - the strip keeps showing them (their .idle-client class
        // turns the presence dot amber)
        let rows = document.querySelectorAll("#channel-list-container .connected-client, #idle-clients-container .connected-client");

        let previous_scroll = body.scrollTop;
        let fragment = document.createDocumentFragment();
        let count = 0;

        // which conversation is on screen decides which circle wears the active ring:
        // a pm context rings that user's circle, anything else rings the channel circle
        let active_is_pm = (g_current_chat_context_id != null && g_current_chat_context_id.indexOf("chat-context-pm-") == 0);
        let active_pm_client_id = (active_is_pm == true) ? g_current_chat_context_id.split("chat-context-pm-")[1] : null;

        // a conversation with somebody OFFLINE is a third kind of context. without this the
        // ring stayed on the channel circle while you were writing to an offline person
        let active_is_offline = (g_current_chat_context_id != null && g_current_chat_context_id.indexOf("chat-context-offline-") == 0);
        let active_offline_alias = (active_is_offline == true) ? g_current_chat_context_id.substring("chat-context-offline-".length) : "";

        // the current channel leads the strip as its own circle ("the room you are in").
        // taps on it are forwarded at init (member-strip forwarding): tap = show the
        // channel chat, hold = the channel switch list
        let current_channel_row = (tree != null) ? tree.querySelector('.single-channel[data-channel-id="' + current_channel_id + '"]') : null;
        if (current_channel_row != null)
        {
            let channel_name_element = current_channel_row.querySelector("[data-channel-name-id]");

            let channel_circle = document.createElement("div");
            channel_circle.className = "member-list-channel";
            channel_circle.setAttribute("data-member-list-channel-id", current_channel_id);

            if (active_is_pm == false && active_is_offline == false)
            {
                channel_circle.classList.add("member-list-context-active");
            }

            let channel_icon = document.createElement("div");
            channel_icon.className = "member-list-channel-icon";

            // channels with an uploaded icon show it; the rest get the css "#" glyph
            let tree_icon = current_channel_row.querySelector(".single-channel-icon");
            if (tree_icon != null && tree_icon.style.backgroundImage != "")
            {
                channel_icon.style.backgroundImage = tree_icon.style.backgroundImage;
                channel_icon.classList.add("has-icon");
            }

            let channel_name = document.createElement("p");
            channel_name.className = "member-list-channel-name";
            channel_name.textContent = (channel_name_element != null) ? channel_name_element.textContent : "";

            // unread counter on the circle itself. the strip themes hide the channel
            // tree entirely, so the badge in those rows can never be seen there - this
            // is the only channel surface bluebell/simpledark actually show
            let channel_unread = document.createElement("p");
            let channel_unread_count = get_channel_unread_count(current_channel_id);
            channel_unread.className = "member-list-channel-unread"
                + ((channel_unread_count > 0) ? "" : " unread-empty");
            channel_unread.textContent = channel_unread_count;

            channel_circle.appendChild(channel_icon);
            channel_circle.appendChild(channel_name);
            channel_circle.appendChild(channel_unread);
            fragment.appendChild(channel_circle);
        }

        for (let i = 0; i < rows.length; i++)
        {
            let clone = rows[i].cloneNode(true);

            // ring the circle of the user whose private chat is on screen
            if (active_is_pm == true && rows[i].getAttribute("data-connected-client-id") == active_pm_client_id)
            {
                clone.classList.add("member-list-context-active");
            }

            if (clone.hasAttribute("id")) { clone.removeAttribute("id"); }
            let ided = clone.querySelectorAll("[id]");
            for (let j = 0; j < ided.length; j++) { ided[j].removeAttribute("id"); }

            clone.classList.remove("collapsed");
            // distinct class so the right member list can be styled independently of the tree rows
            // (re-added on every sync, since each clone is a fresh copy of a tree row)
            clone.classList.add("member-list-client");

            // the local-client row carries the name in an editable <input>; swap it for text
            let inputs = clone.getElementsByTagName("input");
            while (inputs.length > 0)
            {
                let inp = inputs[0];
                let p = document.createElement("p");
                p.className = "connected-client-p";
                p.textContent = inp.value;
                inp.parentNode.replaceChild(p, inp);
            }

            // paint this client's avatar (if any) onto the clone's circle. avatars live only in
            // the member list; the left tree keeps its plain circle + green dot + speaking ring
            if (g_avatars_allowed == true)
            {
                let cid = rows[i].getAttribute("data-connected-client-id");
                let client_object = (cid != null) ? get_client_by_client_id(cid) : null;
                if (client_object != null && typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0)
                {
                    let circle = clone.querySelector(".client-audio-state");
                    if (circle != null)
                    {
                        circle.style.backgroundImage = "url(" + client_object.base64_avatar + ")";
                        circle.style.backgroundSize = "cover";
                    }
                }
            }

            // no avatar: fall back to the first letters of the display name, painted as a
            // label on the circle. purely local decoration - nothing is sent or stored
            let clone_circle = clone.querySelector(".client-audio-state");
            if (clone_circle != null && clone_circle.style.backgroundImage == "")
            {
                let clone_name_element = clone.querySelector(".has-alias .client-alias, .client-alias, .connected-client-p");
                if (clone.classList.contains("has-alias") == false)
                {
                    clone_name_element = clone.querySelector(".connected-client-p");
                }
                clone_circle.setAttribute("data-initials", UI.initials_for_name((clone_name_element != null) ? clone_name_element.textContent : ""));
            }

            // second caption under the name: which channel this person sits in
            // (the strip is the only channel ui in those themes, so this is how you see
            // where everyone is). offline contacts have no live channel - skipped.
            let caption_client_object = get_client_by_client_id(rows[i].getAttribute("data-connected-client-id"));
            if (caption_client_object != null)
            {
                let caption_channel = get_channel_by_id(g_channel_list, caption_client_object.channel_id);
                if (caption_channel != null)
                {
                    let channel_caption = document.createElement("p");
                    channel_caption.className = "member-list-client-channel";
                    channel_caption.textContent = caption_channel.name;
                    clone.appendChild(channel_caption);
                }

                // the "neighbors only" toggle hides everyone outside
                // the local client's channel - this class is what it keys on
                if (parseInt(caption_client_object.channel_id) == parseInt(current_channel_id))
                {
                    clone.classList.add("in-current-channel");
                }
            }

            fragment.appendChild(clone);
            count++;
        }

        // offline people: everyone the server has stored whose alias is NOT among the aliases
        // currently connected. the owner joining makes his live row appear above and drops his
        // offline copy here, so nobody is ever listed twice
        let offline_count = 0;

        if (g_offline_client_list.length > 0)
        {
            let online_aliases = new Set();
            for (let i = 0; i < g_client_list.length; i++)
            {
                if (typeof g_client_list[i].alias === "string" && g_client_list[i].alias.length > 0)
                {
                    online_aliases.add(g_client_list[i].alias.toLowerCase());
                }
            }

            for (let i = 0; i < g_offline_client_list.length; i++)
            {
                let stored_client = g_offline_client_list[i];

                if (online_aliases.has(stored_client.alias.toLowerCase()) == true)
                {
                    continue; // this person is connected, his live row is already listed
                }

                let row = document.createElement("div");
                row.className = "connected-client member-list-client offline-contact";

                // the conversation on screen is with this offline person: ring their circle,
                // the same marker a channel or a live private chat gets
                if (active_is_offline == true && stored_client.alias.toLowerCase() == active_offline_alias.toLowerCase())
                {
                    row.classList.add("member-list-context-active");
                }

                // cloned rows start with the tree's indent spacer, and themes hide that
                // first child. without a spacer of our own the CIRCLE would be first and
                // get hidden instead (that is exactly what happened in termix)
                let indent_spacer = document.createElement("div");
                indent_spacer.style.width = "0px";
                indent_spacer.style.height = "1px";
                indent_spacer.style.display = "inline-block";
                row.appendChild(indent_spacer);

                let circle = document.createElement("div");
                circle.className = "client-audio-state";
                if (g_avatars_allowed == true && stored_client.base64_avatar.length > 0)
                {
                    circle.style.backgroundImage = "url(" + stored_client.base64_avatar + ")";
                    circle.style.backgroundSize = "cover";
                }
                else
                {
                    circle.setAttribute("data-initials", UI.initials_for_name(stored_client.alias));
                }
                row.appendChild(circle);

                // the alias is the display name; themes that show aliases key off .has-alias
                let name = document.createElement("p");
                name.className = "client-alias";
                name.textContent = stored_client.alias;
                row.classList.add("has-alias");
                row.appendChild(name);

                fragment.appendChild(row);
                offline_count++;
            }
        }

        body.innerHTML = "";
        if (fragment.childNodes.length == 0)
        {
            let empty = document.createElement("div");
            empty.id = "member-list-empty";
            empty.textContent = "no one connected";
            body.appendChild(empty);
        }
        else
        {
            body.appendChild(fragment);
        }

        body.scrollTop = previous_scroll;
        if (header != null) { header.textContent = "members — " + count; }
    },

    // flatten toggle (shown only by termix): flips a class on the tree; the theme's css zeroes
    // every row's indent while it is set, so newly-arriving channels render flat too
    channel_flatten_toggle_onclick: function()
    {
        g_is_channel_list_flattened = !g_is_channel_list_flattened;
        UI.apply_channel_flatten_state();
        try { localStorage.setItem("lemon_channels_flat", g_is_channel_list_flattened ? "1" : "0"); } catch (e) { console.warn("failed to save channel-flatten preference:", e.message); }
    },

    // paints the current g_is_channel_list_flattened flag onto the tree (the class the css
    // keys on) and onto the toggle button's active state
    apply_channel_flatten_state: function()
    {
        let tree = document.getElementById("channel-list-container");
        if (tree != null)
        {
            if (g_is_channel_list_flattened == true) { tree.classList.add("channels-flattened"); }
            else { tree.classList.remove("channels-flattened"); }
        }

        let button = document.getElementById("channel-flatten-toggle-button");
        if (button != null)
        {
            if (g_is_channel_list_flattened == true) { button.classList.add("flatten-active"); }
            else { button.classList.remove("flatten-active"); }
        }
    },

    // touch devices may only use themes tagged data-mobile="true" in the picker (the others are
    // not laid out for a phone); desktop may use anything. an unknown theme name is allowed on
    // desktop but rejected on a phone
    is_theme_allowed_on_this_device: function(themename)
    {
        if (!g_is_client_running_under_touch_device)
        {
            return true;
        }

        let theme_items = document.getElementsByClassName("choose-theme-item");
        for (let i = 0; i < theme_items.length; i++)
        {
            if (theme_items[i].getAttribute("data-name") === themename)
            {
                return theme_items[i].getAttribute("data-mobile") === "true";
            }
        }
        return false;
    },

    // theme picked in the settings list: applies (and persists) it by its data-name
    choose_theme_item_onclick: function(event)
    {
        event.stopPropagation();
        UI.apply_theme(event.target.getAttribute("data-name"));
    },

    // hide/show-chat button: on touch devices it resizes the space-devider panels directly; on
    // desktop it only flips g_is_chat_hidden and layout_apply() rebalances the grid. swaps the icon
    hide_chat_container: function()
    {
        if (g_is_client_running_under_touch_device)
        {
            if (g_is_chat_hidden == false)
            {
                document.getElementById("space-devider2").style.display = "none";
                document.getElementById("space-devider1").style.width = "100%";
                document.getElementById("space-devider4").style.display = "none";
                document.getElementById("hide-chat-button").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAGXRFWHRTb2Z0d2FyZQBBZG9iZSBJbWFnZVJlYWR5ccllPAAAA2RpVFh0WE1MOmNvbS5hZG9iZS54bXAAAAAAADw/eHBhY2tldCBiZWdpbj0i77u/IiBpZD0iVzVNME1wQ2VoaUh6cmVTek5UY3prYzlkIj8+IDx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IkFkb2JlIFhNUCBDb3JlIDUuMC1jMDYwIDYxLjEzNDc3NywgMjAxMC8wMi8xMi0xNzozMjowMCAgICAgICAgIj4gPHJkZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4gPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIgeG1sbnM6eG1wTU09Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9tbS8iIHhtbG5zOnN0UmVmPSJodHRwOi8vbnMuYWRvYmUuY29tL3hhcC8xLjAvc1R5cGUvUmVzb3VyY2VSZWYjIiB4bWxuczp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iIHhtcE1NOk9yaWdpbmFsRG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIgeG1wTU06RG9jdW1lbnRJRD0ieG1wLmRpZDozRjA1RkVDNEZDMjIxMUUwQUE3NEQwMjJDRUE1NTVGNyIgeG1wTU06SW5zdGFuY2VJRD0ieG1wLmlpZDozRjA1RkVDM0ZDMjIxMUUwQUE3NEQwMjJDRUE1NTVGNyIgeG1wOkNyZWF0b3JUb29sPSJBZG9iZSBQaG90b3Nob3AgQ1M1IFdpbmRvd3MiPiA8eG1wTU06RGVyaXZlZEZyb20gc3RSZWY6aW5zdGFuY2VJRD0ieG1wLmlpZDoxRjhEQ0Y5MDhFRjNFMDExQTE2NUI2RjQzNUVGQkUzNyIgc3RSZWY6ZG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIvPiA8L3JkZjpEZXNjcmlwdGlvbj4gPC9yZGY6UkRGPiA8L3g6eG1wbWV0YT4gPD94cGFja2V0IGVuZD0iciI/PieHf0QAAANvSURBVHja7FbfS1NRHD+7G9PpcAoOJ9Nhom4urg7aY6A4CqHnKIii8D8o6imIpF4SfTSEQhBMXw0mIxwkBD35A6bTKBR/TMTBdOoczk37fE/3rKtOSXSshx343nPO93vO+Xzu98e5V3N0dMRy2SSW45YnkCeQJ5AnkHMCurGxsdx7oKCggFVWVrKioqKsAwosvV7/xwP0sNvt10pLS9sPDw/HZ2ZmgltbW1kBNxqNzOVy2SRJulNeXv45EAiEuAfwRexH1wvDVH19fZtGo8kKAYfDcQMYPwlLq9UOpEOwtLSUUD7LeoPB0FddXa3Pguu1y8vLg5FIhJ+NsTFNYHt7uzMUCqWUtXU1NTXPLRbLhQAofxBKptPpMtpTqZQVYX67sLDgCwaDDEQ61WX4bXFxsQdKsf5NQ0NDB8LB4Kp/Am9ubnZWVFT0ozcLErSXko7m0MfxUi3on8Xj8bswezkBJB4jAcOXSEDf5uYmU3QfcGCv2+02wSOspKRE6E9JVVWVDLCvGD9GCP2yLJsJHMRMyWTSRnPSw95B66xW6y+xV+Pz+dQvYwDbL3jzm2azWejILR+J8cHBwTgRxKEsFotR8vJxNBodaGpqelhcXCz2BGD3zM/P25F4P6D3QyeTgfYh+z8lEokHNNeMjo6e9KgJ0odN92praxnipraFIcQ4SiAUWtIhob6vra35QUJW3SVkvw8ZFuB7e3sEHgS4B9N1TsDr9Z4VWhfkHYjcKisrYyaTiQvKKNPaVyDxPgOJdBPg8GIanF9EFIcz2jTk9s7OjgvSjnELwNtwOC8jcrfqvnhNrkU4PMhwPy4bWU2UQjU3Nxfc398/Bs49MDIycpFq0wt3Kh5Sl0gA+TOBhKsDySllbboK4QE3knwaHjj+MTrHA5laAjKhjCeOHYRSczqdZpTdMCrq5EWmhX6wsbHRMzs7u64mIZ1VWhcRKjkCLywspFKTSbe7u8smJyd5r6xzkh0kLLRe7JUoPpcVm812CpwSDnlxHX3gJAmst4i9EtzFLisrKyvdSDCZxkhYDo55K+Y88YgE6cmOuXN1dbVb7L2SPyIkWBcSLLyxscEQYyq1VuXO4HcHlR7pw+Ew2SPwTFe6CoaGhq7qg0fV8RTyQgWubnS1dkN6lBL/+0NyRY1uvifn2InUo//up/S3AAMAiJlus3so8OYAAAAASUVORK5CYII=)";
                g_is_chat_hidden = true;
            }
            else
            {
                document.getElementById("space-devider2").style.display = "inline-block";
                document.getElementById("space-devider4").style.display = "inline-block";
                document.getElementById("space-devider1").style.width = "30%";
                document.getElementById("space-devider2").style.width = "100%";
                document.getElementById("hide-chat-button").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAGXRFWHRTb2Z0d2FyZQBBZG9iZSBJbWFnZVJlYWR5ccllPAAAA2RpVFh0WE1MOmNvbS5hZG9iZS54bXAAAAAAADw/eHBhY2tldCBiZWdpbj0i77u/IiBpZD0iVzVNME1wQ2VoaUh6cmVTek5UY3prYzlkIj8+IDx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IkFkb2JlIFhNUCBDb3JlIDUuMC1jMDYwIDYxLjEzNDc3NywgMjAxMC8wMi8xMi0xNzozMjowMCAgICAgICAgIj4gPHJkZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4gPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIgeG1sbnM6eG1wTU09Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9tbS8iIHhtbG5zOnN0UmVmPSJodHRwOi8vbnMuYWRvYmUuY29tL3hhcC8xLjAvc1R5cGUvUmVzb3VyY2VSZWYjIiB4bWxuczp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iIHhtcE1NOk9yaWdpbmFsRG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIgeG1wTU06RG9jdW1lbnRJRD0ieG1wLmRpZDo5MkQ4RDIzRUYzOEUxMUUwOTZDOEQ1RTU1NDQ3N0RBMSIgeG1wTU06SW5zdGFuY2VJRD0ieG1wLmlpZDo5MkQ4RDIzREYzOEUxMUUwOTZDOEQ1RTU1NDQ3N0RBMSIgeG1wOkNyZWF0b3JUb29sPSJBZG9iZSBQaG90b3Nob3AgQ1M1IFdpbmRvd3MiPiA8eG1wTU06RGVyaXZlZEZyb20gc3RSZWY6aW5zdGFuY2VJRD0ieG1wLmlpZDo5N0U1MkY5MjhFRjNFMDExQTE2NUI2RjQzNUVGQkUzNyIgc3RSZWY6ZG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIvPiA8L3JkZjpEZXNjcmlwdGlvbj4gPC9yZGY6UkRGPiA8L3g6eG1wbWV0YT4gPD94cGFja2V0IGVuZD0iciI/PuSt+6UAAASASURBVHja7FddSFtXHD/xxu9oNBq5Or1Svy0G5joLwsbGoEVxT6W1sD2VwQp9LKwMCn0rfZCWCWMwYaxQkIqZb4JMJx2DjRUigWikLihGo9H4gRqNRqP7/e7OtWmWalc2wqAHfjn3fPw/zv/rnJiOjo5EKpvpjQKpVsA8MjKSUgXSRIqb+Z8SZGZmivT0dL3PyMjQ56LRqNjb2xP7+/t6/68qkJaWJgoLC4XNZtP7rKysM5i2AW8BqtwWBALA2u7u7sz6+rpYW1sT7A8PD19PAbPZLDRNE6qqWvD9EabagA5AO4khFPSXlpYOAkMHBwejwWAwPDc3p1snaRYMDw//bbKiooLC7YqifInhZ4D1ZQKXlpbE5uamKC4u1i2U0DaA72KxWJff7w9SkRMVoF8bGhoUq9X6BYa3AYuxRlPSHfFtenpazM/PP8Tnb8DlxsbGC3a7PdneMHAXinZNTk7G4uPkeJfFYhHNzc0ahD/B8J4hnL4cHx8XqBeRRO1DodAtdNeAHuDi8vJyH+exN0oa0hrsyTM/P/8XysjLy3uuAAsRT+5wOM4hun/H+D3OwWwC2gqPxzMEV3yME8U4Hw/QORPGveyxN0oa0k5MTOj+l3taKaOpqek8Zep7+VNTU6OB4Cd8qxzv7OyIsbGxCPz7KcbtCCwrekuiAtXV1VpOTo7+Dcsxds7INYukaYeVrrhcrg00g06FrB+rqqo0js38yc3N/RZ+0wON/gGBDxa4hKFHWqo+WTplZ2d/A5NewacXYKbcidtXL8u8MxKJuNxu9+OWlhb95AxquPwrrF8ykwCB1MqUk+kXgAnfhdk2DE5gkPGSfD4LTCRbSKCZgeAPwfsPzLF+iEAg8IEerNRydna2OxwO69FLE1VWVr5g7tXV1RnGBNdfBdxLmnge4Gkjb65TFmR+rceAJLg7NTXllQyUoqKi+wUFBccMcZqhxcXF2KsqwL2kMcbkRZ7kzTFkeSjz2AJMGwTJdQSMweQqAqyDqSnX/T6fr2dra+tU4dzDvaQhLXmQF3lynYULsm5QJteVzs7OYyGo3e9A23qWYZPJ1AatnyAWAtI9o8jzDkS3CojEjCBWVlaE1+t143RX4eaDkpISUVtbex4x9QPWs7e3t5naj7DebdCYBgYG4mPHimD5ta6u7qwsFqxgt0DYQ7OisFhQ379H6l2GcvqtyMY8h8+Zvk4ofw0XV7isrEzBvs+xfJ8Jw9SGch7cnO/LEv1XKXY6nYkBrOH0j8vLy1sBWoJzbqAbGASjEMzsYOmFn1WZjrwNnVDaA6F2eWndBByyYrJs0+/t8tZ8fhf09/cnyyKFOQ3/3UZxURhEcc0jaz/bMyPnZd9qCGXjdbywsMDLqhfD69KiL15GfX19J92uZHgHD4823nY0O4PqpMZ4YSwQcM0gprqAn1/3QcKTtsNvKk7yCYHxOVRO/b0gq5pePREbgkGG9hTolQid+iI67cUS9+J5IKEgBt6Wb4Qaue6TgcVYif2nb0IpwCW/R//3r+KUK/Dmr1nKFfhTgAEA1AVXSbHzbjUAAAAASUVORK5CYII=)";
                g_is_chat_hidden = false;
            }
        }
        else
        {
            if (g_is_chat_hidden == false)
            {
                // the grid layout hides the chat+input panels and rebalances the columns
                // in layout_apply(), called at the end of this function once the flag is set
                document.getElementById("hide-chat-button").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAGXRFWHRTb2Z0d2FyZQBBZG9iZSBJbWFnZVJlYWR5ccllPAAAA2RpVFh0WE1MOmNvbS5hZG9iZS54bXAAAAAAADw/eHBhY2tldCBiZWdpbj0i77u/IiBpZD0iVzVNME1wQ2VoaUh6cmVTek5UY3prYzlkIj8+IDx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IkFkb2JlIFhNUCBDb3JlIDUuMC1jMDYwIDYxLjEzNDc3NywgMjAxMC8wMi8xMi0xNzozMjowMCAgICAgICAgIj4gPHJkZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4gPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIgeG1sbnM6eG1wTU09Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9tbS8iIHhtbG5zOnN0UmVmPSJodHRwOi8vbnMuYWRvYmUuY29tL3hhcC8xLjAvc1R5cGUvUmVzb3VyY2VSZWYjIiB4bWxuczp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iIHhtcE1NOk9yaWdpbmFsRG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIgeG1wTU06RG9jdW1lbnRJRD0ieG1wLmRpZDozRjA1RkVDNEZDMjIxMUUwQUE3NEQwMjJDRUE1NTVGNyIgeG1wTU06SW5zdGFuY2VJRD0ieG1wLmlpZDozRjA1RkVDM0ZDMjIxMUUwQUE3NEQwMjJDRUE1NTVGNyIgeG1wOkNyZWF0b3JUb29sPSJBZG9iZSBQaG90b3Nob3AgQ1M1IFdpbmRvd3MiPiA8eG1wTU06RGVyaXZlZEZyb20gc3RSZWY6aW5zdGFuY2VJRD0ieG1wLmlpZDoxRjhEQ0Y5MDhFRjNFMDExQTE2NUI2RjQzNUVGQkUzNyIgc3RSZWY6ZG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIvPiA8L3JkZjpEZXNjcmlwdGlvbj4gPC9yZGY6UkRGPiA8L3g6eG1wbWV0YT4gPD94cGFja2V0IGVuZD0iciI/PieHf0QAAANvSURBVHja7FbfS1NRHD+7G9PpcAoOJ9Nhom4urg7aY6A4CqHnKIii8D8o6imIpF4SfTSEQhBMXw0mIxwkBD35A6bTKBR/TMTBdOoczk37fE/3rKtOSXSshx343nPO93vO+Xzu98e5V3N0dMRy2SSW45YnkCeQJ5AnkHMCurGxsdx7oKCggFVWVrKioqKsAwosvV7/xwP0sNvt10pLS9sPDw/HZ2ZmgltbW1kBNxqNzOVy2SRJulNeXv45EAiEuAfwRexH1wvDVH19fZtGo8kKAYfDcQMYPwlLq9UOpEOwtLSUUD7LeoPB0FddXa3Pguu1y8vLg5FIhJ+NsTFNYHt7uzMUCqWUtXU1NTXPLRbLhQAofxBKptPpMtpTqZQVYX67sLDgCwaDDEQ61WX4bXFxsQdKsf5NQ0NDB8LB4Kp/Am9ubnZWVFT0ozcLErSXko7m0MfxUi3on8Xj8bswezkBJB4jAcOXSEDf5uYmU3QfcGCv2+02wSOspKRE6E9JVVWVDLCvGD9GCP2yLJsJHMRMyWTSRnPSw95B66xW6y+xV+Pz+dQvYwDbL3jzm2azWejILR+J8cHBwTgRxKEsFotR8vJxNBodaGpqelhcXCz2BGD3zM/P25F4P6D3QyeTgfYh+z8lEokHNNeMjo6e9KgJ0odN92praxnipraFIcQ4SiAUWtIhob6vra35QUJW3SVkvw8ZFuB7e3sEHgS4B9N1TsDr9Z4VWhfkHYjcKisrYyaTiQvKKNPaVyDxPgOJdBPg8GIanF9EFIcz2jTk9s7OjgvSjnELwNtwOC8jcrfqvnhNrkU4PMhwPy4bWU2UQjU3Nxfc398/Bs49MDIycpFq0wt3Kh5Sl0gA+TOBhKsDySllbboK4QE3knwaHjj+MTrHA5laAjKhjCeOHYRSczqdZpTdMCrq5EWmhX6wsbHRMzs7u64mIZ1VWhcRKjkCLywspFKTSbe7u8smJyd5r6xzkh0kLLRe7JUoPpcVm812CpwSDnlxHX3gJAmst4i9EtzFLisrKyvdSDCZxkhYDo55K+Y88YgE6cmOuXN1dbVb7L2SPyIkWBcSLLyxscEQYyq1VuXO4HcHlR7pw+Ew2SPwTFe6CoaGhq7qg0fV8RTyQgWubnS1dkN6lBL/+0NyRY1uvifn2InUo//up/S3AAMAiJlus3so8OYAAAAASUVORK5CYII=)";
                g_is_chat_hidden = true;
            }
            else
            {
                // panel visibility and column sizes come back via layout_apply() below
                document.getElementById("hide-chat-button").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAGXRFWHRTb2Z0d2FyZQBBZG9iZSBJbWFnZVJlYWR5ccllPAAAA2RpVFh0WE1MOmNvbS5hZG9iZS54bXAAAAAAADw/eHBhY2tldCBiZWdpbj0i77u/IiBpZD0iVzVNME1wQ2VoaUh6cmVTek5UY3prYzlkIj8+IDx4OnhtcG1ldGEgeG1sbnM6eD0iYWRvYmU6bnM6bWV0YS8iIHg6eG1wdGs9IkFkb2JlIFhNUCBDb3JlIDUuMC1jMDYwIDYxLjEzNDc3NywgMjAxMC8wMi8xMi0xNzozMjowMCAgICAgICAgIj4gPHJkZjpSREYgeG1sbnM6cmRmPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5LzAyLzIyLXJkZi1zeW50YXgtbnMjIj4gPHJkZjpEZXNjcmlwdGlvbiByZGY6YWJvdXQ9IiIgeG1sbnM6eG1wTU09Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC9tbS8iIHhtbG5zOnN0UmVmPSJodHRwOi8vbnMuYWRvYmUuY29tL3hhcC8xLjAvc1R5cGUvUmVzb3VyY2VSZWYjIiB4bWxuczp4bXA9Imh0dHA6Ly9ucy5hZG9iZS5jb20veGFwLzEuMC8iIHhtcE1NOk9yaWdpbmFsRG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIgeG1wTU06RG9jdW1lbnRJRD0ieG1wLmRpZDo5MkQ4RDIzRUYzOEUxMUUwOTZDOEQ1RTU1NDQ3N0RBMSIgeG1wTU06SW5zdGFuY2VJRD0ieG1wLmlpZDo5MkQ4RDIzREYzOEUxMUUwOTZDOEQ1RTU1NDQ3N0RBMSIgeG1wOkNyZWF0b3JUb29sPSJBZG9iZSBQaG90b3Nob3AgQ1M1IFdpbmRvd3MiPiA8eG1wTU06RGVyaXZlZEZyb20gc3RSZWY6aW5zdGFuY2VJRD0ieG1wLmlpZDo5N0U1MkY5MjhFRjNFMDExQTE2NUI2RjQzNUVGQkUzNyIgc3RSZWY6ZG9jdW1lbnRJRD0ieG1wLmRpZDpERDc0QzMwNzA5MjA2ODExQTRFMEQzRDI2RDUyMTgzQyIvPiA8L3JkZjpEZXNjcmlwdGlvbj4gPC9yZGY6UkRGPiA8L3g6eG1wbWV0YT4gPD94cGFja2V0IGVuZD0iciI/PuSt+6UAAASASURBVHja7FddSFtXHD/xxu9oNBq5Or1Svy0G5joLwsbGoEVxT6W1sD2VwQp9LKwMCn0rfZCWCWMwYaxQkIqZb4JMJx2DjRUigWikLihGo9H4gRqNRqP7/e7OtWmWalc2wqAHfjn3fPw/zv/rnJiOjo5EKpvpjQKpVsA8MjKSUgXSRIqb+Z8SZGZmivT0dL3PyMjQ56LRqNjb2xP7+/t6/68qkJaWJgoLC4XNZtP7rKysM5i2AW8BqtwWBALA2u7u7sz6+rpYW1sT7A8PD19PAbPZLDRNE6qqWvD9EabagA5AO4khFPSXlpYOAkMHBwejwWAwPDc3p1snaRYMDw//bbKiooLC7YqifInhZ4D1ZQKXlpbE5uamKC4u1i2U0DaA72KxWJff7w9SkRMVoF8bGhoUq9X6BYa3AYuxRlPSHfFtenpazM/PP8Tnb8DlxsbGC3a7PdneMHAXinZNTk7G4uPkeJfFYhHNzc0ahD/B8J4hnL4cHx8XqBeRRO1DodAtdNeAHuDi8vJyH+exN0oa0hrsyTM/P/8XysjLy3uuAAsRT+5wOM4hun/H+D3OwWwC2gqPxzMEV3yME8U4Hw/QORPGveyxN0oa0k5MTOj+l3taKaOpqek8Zep7+VNTU6OB4Cd8qxzv7OyIsbGxCPz7KcbtCCwrekuiAtXV1VpOTo7+Dcsxds7INYukaYeVrrhcrg00g06FrB+rqqo0js38yc3N/RZ+0wON/gGBDxa4hKFHWqo+WTplZ2d/A5NewacXYKbcidtXL8u8MxKJuNxu9+OWlhb95AxquPwrrF8ykwCB1MqUk+kXgAnfhdk2DE5gkPGSfD4LTCRbSKCZgeAPwfsPzLF+iEAg8IEerNRydna2OxwO69FLE1VWVr5g7tXV1RnGBNdfBdxLmnge4Gkjb65TFmR+rceAJLg7NTXllQyUoqKi+wUFBccMcZqhxcXF2KsqwL2kMcbkRZ7kzTFkeSjz2AJMGwTJdQSMweQqAqyDqSnX/T6fr2dra+tU4dzDvaQhLXmQF3lynYULsm5QJteVzs7OYyGo3e9A23qWYZPJ1AatnyAWAtI9o8jzDkS3CojEjCBWVlaE1+t143RX4eaDkpISUVtbex4x9QPWs7e3t5naj7DebdCYBgYG4mPHimD5ta6u7qwsFqxgt0DYQ7OisFhQ379H6l2GcvqtyMY8h8+Zvk4ofw0XV7isrEzBvs+xfJ8Jw9SGch7cnO/LEv1XKXY6nYkBrOH0j8vLy1sBWoJzbqAbGASjEMzsYOmFn1WZjrwNnVDaA6F2eWndBByyYrJs0+/t8tZ8fhf09/cnyyKFOQ3/3UZxURhEcc0jaz/bMyPnZd9qCGXjdbywsMDLqhfD69KiL15GfX19J92uZHgHD4823nY0O4PqpMZ4YSwQcM0gprqAn1/3QcKTtsNvKk7yCYHxOVRO/b0gq5pePREbgkGG9hTolQid+iI67cUS9+J5IKEgBt6Wb4Qaue6TgcVYif2nb0IpwCW/R//3r+KUK/Dmr1nKFfhTgAEA1AVXSbHzbjUAAAAASUVORK5CYII=)";
                g_is_chat_hidden = false;
            }
        }

        // desktop runs the grid layout: it derives panel visibility and column sizes
        // from g_is_chat_hidden. no-op on touch devices (the grid is inactive there)
        layout_apply();
        sync_toolbar_state_classes();
    },

    // closes the channel-password dialog and its background overlay
    enter_channel_password_closebutton_onclick: function()
    {
        document.getElementById("channel-password-enter-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
    },

    // closes the admin-password dialog and its background overlay
    close_admin_password_context_button: function()
    {
        document.getElementById("admin-password-enter-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
    },

    // closes the create/edit channel dialog and its background overlay
    channel_properties_closebutton_onclick: function()
    {
        document.getElementById("channel-properties-edit-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
    },

    // save in the edit-channel dialog: validates the name, sends edit_channel_request with
    // all form fields for selected_channel_id, then closes and clears the dialog
    edit_channel_button_onclick: function()
    {
        if (typeof window === 'undefined')
        {
            return;
        }
        let parent_channel_id = document.getElementById("create-channel-hidden-parent-id").value;
        let channel_name = document.getElementById("channel-properties-input-channel-name").value;

        if (channel_name.length == 0)
        {
            custom_alert("empty channel name ?");
            return;
        }

        let channel_description = document.getElementById("channel-properties-input-channel-description").value;
        let channel_password = document.getElementById("channel-properties-input-channel-password").value;
        let is_audio_enabled = document.getElementById("channel-properties-disable-audio-checkbox").checked == false;
        let is_client_limit_active = document.getElementById("channel-properties-limit-clients-checkbox").checked;
        let max_client_count = parseInt(document.getElementById("channel-properties-input-max-clients").value);
        if (isNaN(max_client_count) || max_client_count < 1)
        {
            max_client_count = 0;
        }

        let message_object = {
            message: {
                type: "edit_channel_request",
                channel_id: parseInt(selected_channel_id),
                channel_name: channel_name,
                channel_description: channel_description,
                channel_password: channel_password,
                is_audio_enabled: is_audio_enabled,
                is_client_limit_active: is_client_limit_active,
                max_client_count: max_client_count
            }
        };

        send_message_object(message_object);

        document.getElementById("channel-properties-edit-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
        document.getElementById("channel-properties-input-channel-name").value = "";
        document.getElementById("channel-properties-input-channel-description").value = "";
        document.getElementById("channel-properties-input-channel-password").value = "";
    },

    // create in the channel dialog: validates the name, sends create_channel_request under
    // the stored parent channel id, then closes and clears the dialog
    create_channel_button_onclick: function()
    {
        if (typeof window === 'undefined')
        {
            return;
        }

        let parent_channel_id = parseInt(document.getElementById("create-channel-hidden-parent-id").value);
        let channel_name = document.getElementById("channel-properties-input-channel-name").value;

        if (channel_name.length == 0)
        {
            custom_alert("empty channel name ?");
            return;
        }

        let channel_description = document.getElementById("channel-properties-input-channel-description").value;
        let channel_password = document.getElementById("channel-properties-input-channel-password").value;
        let is_audio_enabled = document.getElementById("channel-properties-disable-audio-checkbox").checked == false;
        let is_client_limit_active = document.getElementById("channel-properties-limit-clients-checkbox").checked;
        let max_client_count = parseInt(document.getElementById("channel-properties-input-max-clients").value);
        if (isNaN(max_client_count) || max_client_count < 1)
        {
            max_client_count = 0;
        }

        let message_object = {
            message: {
                type: "create_channel_request",
                channel_name: channel_name,
                channel_description: channel_description,
                channel_password: channel_password,
                parent_channel_id: parent_channel_id,
                is_audio_enabled: is_audio_enabled,
                is_client_limit_active: is_client_limit_active,
                max_client_count: max_client_count
            }
        };

        console.log(message_object);

        send_message_object(message_object);

        document.getElementById("channel-properties-edit-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
        document.getElementById("channel-properties-input-channel-name").value = "";
        document.getElementById("channel-properties-input-channel-description").value = "";
        document.getElementById("channel-properties-input-channel-password").value = "";
    },

    // the max-clients number is only shown when the "client limit" checkbox is ticked
    refresh_channel_limit_input_visibility: function()
    {
        let checked = document.getElementById("channel-properties-limit-clients-checkbox").checked;
        document.getElementById("channel-properties-input-max-clients").style.display = checked ? "inline-block" : "none";
    },

    // recompute the red "full" state for every channel (count excludes music bots, matching the
    // server). called whenever channel membership or a channel's limit changes
    refresh_all_channel_fullness: function()
    {
        for (let i = 0; i < g_channel_list.length; i++)
        {
            let ch = g_channel_list[i];
            let is_full = false;

            if (ch.is_client_limit_active == true && ch.max_client_count == 0)
            {
                // a limit-active channel with capacity 0 is admin-only, so it is always shown as full
                is_full = true;
            }
            else if (ch.is_client_limit_active == true)
            {
                let count = 0;
                for (let j = 0; j < g_client_list.length; j++)
                {
                    if (g_client_list[j].channel_id == ch.channel_id && g_client_list[j].is_music_bot != true)
                    {
                        count++;
                    }
                }
                is_full = (count >= ch.max_client_count);
            }

            let row = document.querySelector('[data-channel-id="' + ch.channel_id + '"]');
            if (row == null || row.children[2] == null)
            {
                continue;
            }

            // chevron is shown only when the channel has a subchannel or a client
            // (TS3: leaf channels have no expand/collapse arrow). everything is linked by id in the objects.
            let has_children = false;
            for (let k = 0; k < g_channel_list.length; k++)
            {
                if (g_channel_list[k].parent_channel_id == ch.channel_id)
                {
                    has_children = true;
                    break;
                }
            }
            if (has_children == false)
            {
                for (let k = 0; k < g_client_list.length; k++)
                {
                    if (g_client_list[k].channel_id == ch.channel_id)
                    {
                        has_children = true;
                        break;
                    }
                }
            }
            if (has_children == true)
            {
                row.classList.remove("single-channel-no-children");
            }
            else
            {
                row.classList.add("single-channel-no-children");
            }

            let name_el = row.children[2];
            if (is_full == true && name_el.classList.contains("single-channel-is-full") == false)
            {
                name_el.classList.add("single-channel-is-full");
            }
            else if (is_full == false && name_el.classList.contains("single-channel-is-full") == true)
            {
                name_el.classList.remove("single-channel-is-full");
            }
        }
    },

    // server settings nav: marks the clicked tab selected and shows its pane; the identities,
    // general and bans tabs also ask the server for their live data (admin-only replies)
    server_settings_tab_li_onclick: function()
    {
        let action = event.target.getAttribute("data-action");

        // mark which tab is open - the nav had no selected state at all, so nothing told
        // you where you were once the pane changed
        let nav_items = document.getElementsByClassName("server-settings-tab-li");
        for (let i = 0; i < nav_items.length; i++)
        {
            nav_items[i].classList.toggle("server-settings-tab-li-selected", nav_items[i].getAttribute("data-action") == action);
        }

        document.getElementById("server-settings-tab-icons").style.display = "none";
        document.getElementById("server-settings-tab-tags").style.display = "none";
        document.getElementById("server-settings-tab-general-settings").style.display = "none";
        document.getElementById("server-settings-tab-bans").style.display = "none";
        document.getElementById("server-settings-tab-identities").style.display = "none";
        document.getElementById("server-settings-tab-log").style.display = "none";

        if (action == 0)
        {
            document.getElementById("server-settings-tab-icons").style.display = "block";
        }
        else if (action == 1)
        {
            document.getElementById("server-settings-tab-tags").style.display = "block";
        }
        else if (action == 4)
        {
            document.getElementById("server-settings-tab-identities").style.display = "block";

            // pull the live identity list from the server (admins only)
            let message_object = { message: { type: "request_identity_list" } };
            send_message_object(message_object);
        }
        else if (action == 5)
        {
            document.getElementById("server-settings-tab-log").style.display = "block";

            // the settings request fills the four toggle checkboxes, the log request fills the
            // textarea (the server answers both only for admins)
            send_message_object({ message: { type: "load_server_settings" } });
            send_message_object({ message: { type: "admin_log_request" } });
        }
        else if (action == 2 || action == 3)
        {
            if (action == 2)
            {
                document.getElementById("server-settings-tab-general-settings").style.display = "block";
            }
            else
            {
                document.getElementById("server-settings-tab-bans").style.display = "block";
            }

            // ask the server for the current settings + ban list so both tabs reflect the live
            // state (the server only answers admins)
            let message_object = {
                message: {
                    type: "load_server_settings"
                }
            };

            send_message_object(message_object);
        }
    },

    // sends the typed admin password to the server and closes the dialog
    admin_password_send_button_onclick: function()
    {
        let admin_password = document.getElementById("input-admin-password").value;

        let message_object = {
            message: {
                type: "admin_password",
                value: admin_password,
            }
        };

        send_message_object(message_object);
        console.log("admin password request sent");

        document.getElementById("admin-password-enter-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
    },

    // join button in the channel-password dialog: sends join_channel_request with the typed password
    channel_join_button_onclick: function()
    {
        let channel_password = document.getElementById("input-channel-password").value;

        let message_object = {
            message:
            {
                type: "join_channel_request",
                channel_id: parseInt(selected_channel_id),
                channel_password: channel_password
            }
        };

        send_message_object(message_object);
    },

    // avatar file picked: size-checks it against g_avatar_max_upload_bytes, reads it as a
    // data url and sends it to the server as avatar_upload
    choose_avatar_input_onchange: function()
    {
        if (typeof window === 'undefined')
        {
            return;
        }

        if (g_avatars_allowed == false)
        {
            custom_alert("avatars are disabled on this server");
            return;
        }

        let fileInput = document.getElementById('choose_avatar_input');

        let file = fileInput.files[0];

        if (file.size > g_avatar_max_upload_bytes)
        {
            custom_alert("avatar is too large. Max size: " + Math.round(g_avatar_max_upload_bytes / 1024) + "kb");
            return;
        }

        var fileReader = new FileReader();

        if (fileReader && fileInput.files && fileInput.files.length)
        {
            fileReader.onload = function (event)
            {
                let message_object = {
                    message:{
                        type: "avatar_upload",
                        base64_avatar: event.target.result
                    }
                };

                console.log(message_object);

                send_message_object(message_object);
            };

            fileReader.readAsDataURL(file); // invokes onload
        }
    },

    // icon files picked in server settings: size-checks each (5000 bytes), reads them and
    // pushes them onto g_icon_upload_queue for one-at-a-time upload
    choose_icon_input_onchange: function()
    {
        if (typeof window === 'undefined')
        {
            return;
        }

        let fileInput = document.getElementById('choose_icon_input');

        if (!fileInput.files || fileInput.files.length == 0)
        {
            return;
        }

        // queue every selected icon; they are read and then uploaded one at a time (see
        // send_next_queued_icon_upload), each next one only after the previous upload's reply arrives
        let files = Array.prototype.slice.call(fileInput.files);

        for (let i = 0; i < files.length; i++)
        {
            let file = files[i];

            if (file.size > 5000)
            {
                custom_alert("icon '" + file.name + "' is too large. Max size: 5000 bytes");
                continue;
            }

            let fileReader = new FileReader();
            fileReader.onload = function (event)
            {
                g_icon_upload_queue.push(event.target.result);
                send_next_queued_icon_upload();
            };
            fileReader.readAsDataURL(file); // invokes onload
        }

        fileInput.value = ""; // let the same files be chosen again later
    },
    // one-time wiring (guarded by g_settings_delete_delegation_wired) of delegated clicks:
    // tag/icon delete buttons, tag icon boxes, the shared icon picker and its escape/outside closing
    wire_settings_delete_delegation: function()
    {
        if (g_settings_delete_delegation_wired == true)
        {
            return;
        }

        let tags_container = document.getElementById("server-settings-tab-tags-container");
        let icons_container = document.getElementById("server-settings-tab-icons-container");

        if (tags_container == null || icons_container == null)
        {
            return;
        }

        tags_container.addEventListener("click", function(event)
        {
            let button = event.target.closest(".settings-entry-delete-button");
            if (button != null)
            {
                let entry = button.closest(".server-settings-tag-entry");
                if (entry == null) { return; }
                let tag_id = parseInt(entry.getAttribute("data-tag-id"));
                if (isNaN(tag_id) || tag_id == 0) { return; } // never the admin tag
                UI.send_delete_tag_request(tag_id);
                return;
            }

            let icon_box = event.target.closest(".tag-settings-entry-img");
            if (icon_box != null)
            {
                let entry = icon_box.closest(".server-settings-tag-entry");
                if (entry == null) { return; }
                let tag_id = parseInt(entry.getAttribute("data-tag-id"));
                if (isNaN(tag_id)) { return; }
                UI.open_tag_icon_picker(tag_id);
            }
        });

        icons_container.addEventListener("click", function(event)
        {
            let button = event.target.closest(".settings-entry-delete-button");
            if (button == null) { return; }
            let entry = button.closest(".server-settings-icon-entry");
            if (entry == null) { return; }
            let icon_id = parseInt(entry.getAttribute("data-icon-id"));
            if (isNaN(icon_id)) { return; }
            UI.send_delete_icon_request(icon_id);
        });

        let picker = document.getElementById("tag-icon-picker-popup");
        if (picker != null)
        {
            picker.addEventListener("click", function(event)
            {
                let option = event.target.closest(".tag-icon-picker-option");
                if (option == null) { return; }
                // the icon picker is shared: route the pick to whichever target opened it
                if (g_channel_icon_picker_target_channel_id != null)
                {
                    UI.send_set_channel_icon_request(g_channel_icon_picker_target_channel_id, option.getAttribute("data-picker-icon-id"));
                }
                else
                {
                    UI.send_set_tag_icon_request(g_tag_icon_picker_target_tag_id, option.getAttribute("data-picker-icon-id"));
                }
                UI.close_tag_icon_picker();
            });
        }

        // close the picker on Escape, or a click outside it that is not the click that opened it
        document.addEventListener("keydown", function(event)
        {
            if (event.key == "Escape") { UI.close_tag_icon_picker(); }
        });
        document.addEventListener("click", function(event)
        {
            let open_picker = document.getElementById("tag-icon-picker-popup");
            if (open_picker == null || open_picker.style.display == "none") { return; }
            if (open_picker.contains(event.target)) { return; }
            // the clicks that OPEN the picker also bubble here; without these exemptions the
            // picker closes again in the same event, before it is ever painted
            if (event.target.closest(".tag-settings-entry-img") != null) { return; }
            if (event.target.closest("#channel-properties-icon-box") != null) { return; }
            UI.close_tag_icon_picker();
        });

        g_settings_delete_delegation_wired = true;
    },
    // sends server_settings_delete_tag for the given tag id
    send_delete_tag_request: function(tag_id)
    {
        let message_object = { message: { type: "server_settings_delete_tag", tag_id: tag_id } };
        send_message_object(message_object);
    },
    // sends server_settings_delete_icon for the given icon id
    send_delete_icon_request: function(icon_id)
    {
        let message_object = { message: { type: "server_settings_delete_icon", icon_id: icon_id } };
        send_message_object(message_object);
    },
    // fills the shared icon-picker popup with "none" + every icon in g_icons and opens it
    // targeting a tag (clears any channel target)
    open_tag_icon_picker: function(tag_id)
    {
        let popup = document.getElementById("tag-icon-picker-popup");
        if (popup == null) { console.warn("tag icon picker popup element missing; not opened"); return; }

        g_tag_icon_picker_target_tag_id = tag_id;
        g_channel_icon_picker_target_channel_id = null;

        let html = "<div class=\"tag-icon-picker-option tag-icon-picker-no-icon\" data-picker-icon-id=\"none\" title=\"no icon\">none</div>";
        for (let i = 0; i < g_icons.length; i++)
        {
            html += "<img class=\"tag-icon-picker-option\" src=\"" + g_icons[i].base64_icon + "\" data-picker-icon-id=\"" + g_icons[i].id + "\" title=\"icon " + g_icons[i].id + "\">";
        }
        popup.innerHTML = html;
        popup.style.display = "flex";
    },
    // same picker as g_tags, targeting a channel instead. selecting "none" clears the channel icon
    open_channel_icon_picker: function(channel_id)
    {
        let popup = document.getElementById("tag-icon-picker-popup");
        if (popup == null) { return; }

        g_channel_icon_picker_target_channel_id = channel_id;
        g_tag_icon_picker_target_tag_id = null;

        let html = "<div class=\"tag-icon-picker-option tag-icon-picker-no-icon\" data-picker-icon-id=\"none\" title=\"no icon\">none</div>";
        for (let i = 0; i < g_icons.length; i++)
        {
            html += "<img class=\"tag-icon-picker-option\" src=\"" + g_icons[i].base64_icon + "\" data-picker-icon-id=\"" + g_icons[i].id + "\" title=\"icon " + g_icons[i].id + "\">";
        }
        if (g_icons.length == 0)
        {
            html += "<p style=\"color:#cccccc; font-size:11px; width:100%;\">no icons uploaded yet - add some in server settings</p>";
        }
        popup.innerHTML = html;
        // re-parent to <body> so no themed/transformed/hidden ancestor can swallow it,
        // and float it above the channel edit modal
        document.body.appendChild(popup);
        popup.style.zIndex = "100001";
        popup.style.display = "flex";
    },
    // hides the shared icon picker and forgets both its tag and channel targets
    close_tag_icon_picker: function()
    {
        let popup = document.getElementById("tag-icon-picker-popup");
        if (popup != null) { popup.style.display = "none"; }
        g_tag_icon_picker_target_tag_id = null;
        g_channel_icon_picker_target_channel_id = null;
    },
    // sends server_settings_set_tag_icon; "none" omits icon_id, which clears the tag's icon
    send_set_tag_icon_request: function(tag_id, picked_icon_id)
    {
        if (tag_id == null) { return; }

        let message = { type: "server_settings_set_tag_icon", tag_id: tag_id };
        if (picked_icon_id != "none") { message.icon_id = parseInt(picked_icon_id); } // absent icon_id clears the icon

        let message_object = { message: message };
        send_message_object(message_object);
    },
    // sends set_channel_icon; "none" omits icon_id, which clears the channel's icon
    send_set_channel_icon_request: function(channel_id, picked_icon_id)
    {
        if (channel_id == null) { console.warn("set channel icon: channel_id is null; request not sent"); return; }

        let message = { type: "set_channel_icon", channel_id: parseInt(channel_id) };
        if (picked_icon_id != "none") { message.icon_id = parseInt(picked_icon_id); } // absent icon_id clears the icon

        let message_object = { message: message };
        send_message_object(message_object);
    },
    // paints (or clears) a channel row's icon box from the channel's has_channel_icon/channel_icon_id.
    // the box is styled here rather than in theme css, so it works in every theme with no css edit
    refresh_channel_icon: function(channel)
    {
        if (channel == null) { return; }

        let holder = document.querySelector('.single-channel-icon[data-channel-icon-for="' + channel.channel_id + '"]');
        if (holder == null) { return; }

        let icon = (channel.has_channel_icon == true) ? get_icon_by_icon_id(channel.channel_icon_id) : null;

        if (icon != null)
        {
            holder.style.display = "inline-block";
            holder.style.width = "15px";
            holder.style.height = "15px";
            holder.style.backgroundSize = "contain";
            holder.style.backgroundRepeat = "no-repeat";
            holder.style.backgroundPosition = "center";
            holder.style.verticalAlign = "middle";
            holder.style.marginLeft = "4px";
            holder.style.backgroundImage = "url(" + icon.base64_icon + ")";
        }
        else
        {
            holder.style.display = "none";
            holder.style.backgroundImage = "";
        }
    },
    // repaints the icon box of every channel row from the current g_channel_list
    refresh_all_channel_icons: function()
    {
        for (let i = 0; i < g_channel_list.length; i++)
        {
            UI.refresh_channel_icon(g_channel_list[i]);
        }
    },
    // paints the channel edit form's icon box from the channel's current icon (or clears it)
    refresh_channel_edit_icon_box: function(channel)
    {
        let box = document.getElementById("channel-properties-icon-box");
        if (box == null) { return; }

        let icon = (channel != null && channel.has_channel_icon == true) ? get_icon_by_icon_id(channel.channel_icon_id) : null;
        box.style.backgroundImage = (icon != null) ? ("url(" + icon.base64_icon + ")") : "";
    },

    // mp3 picked for streaming: size-checks it (40mb), announces microphone_usage=1 to the
    // server, then reads the file and hands the buffer to g_minimp3_worker for decoding
    choose_song_file_input_onchange: function()
    {
        if (typeof window === 'undefined')
        {
            return;
        }

        if (g_is_microphone_enabled == false)
        {
            return;
        }

        let fileInput = document.getElementById('choose-song-file-input');

        let file = fileInput.files[0];
        let max_size = 40 * 1024 * 1024;

        if (file.size > max_size)
        {
            custom_alert("file is too large. Max size: 40mb");
            return;
        }

        var fileReader = new FileReader();

        if (fileReader && fileInput.files && fileInput.files.length)
        {
            fileReader.readAsArrayBuffer(fileInput.files[0]);

            g_selected_song_name = fileInput.files[0].name;

            let file_extension = get_file_extension(g_selected_song_name);

            g_is_microphone_active = true;
            let message_object = {
                message: {
                    type: "microphone_usage",
                    value: 1,
                }
            };

            is_playing_music = true;

            send_message_object(message_object);
            g_last_sent_value_microphone_usage = 1;

            fileReader.onload = function (event)
            {
                if (file_extension == "mp3")
                {
                    document.getElementById("custom-file-upload-button-song").style.display = "none";
                    document.getElementById("stop-song").style.display = "block";

                    let arrayBuffer = fileReader.result;

                    g_minimp3_worker.postMessage({
                        type: "decode",
                        value: arrayBuffer
                    });
                }
            };
        }
    },

    // drag handle next to the chat column: starts the grid column drag
    drag_resize_chat_onclick: function(e)
    {
        layout_column_drag_start(e, "chat");
    },

    // opens the admin-password dialog and its background overlay
    enter_admin_password_button_onclick: function()
    {
        document.getElementById("admin-password-enter-container").style.display = "block";
        document.getElementById("background-container").style.display = "block";
    },

    // closes the secret-identity dialog and wipes the shown identity string from the dom
    close_button_secret_identity_string_onclick: function()
    {
        document.getElementById("secret-identity-string-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
        document.getElementById("p-secret-identity-string").innerHTML = "";
    },

    // clears the visible chat: removes every chat and server message element and resets the
    // last-known-sender tracking for the current context
    clear_chat_button_onclick: function()
    {
        let chat_context_index = get_chat_context_index_by_chat_context_id(g_current_chat_context_id);
        g_chat_context_array[chat_context_index].last_known_message_sender_username = "";

        last_known_message_sender_username = "";
        let elements = document.getElementsByClassName('single-chat-message');

        for (let i = 0; i < elements.length; i++)
        {
            elements[i].remove();
        }

        let elements1 = document.getElementsByClassName('single-server-message');

        for (let i = 0; i < elements1.length; i++)
        {
            elements1[i].remove();
        }

        // the file cards went with the messages; the bytes behind them go too
        prune_chat_files_without_cards();
    },

    // sends server_settings_add_new_tag with the typed tag name (an icon is assigned later)
    add_new_tag_to_server_button_onlick: function(event)
    {
        event.stopPropagation();

        let tag_name = document.getElementById("server-settings-tab-add-tag-details-input-tag-name").value;

        // g_tags are created without an icon; an icon is assigned later by clicking the tag's icon box
        let message_object = {
            message: {
                type: "server_settings_add_new_tag",
                tag_name: tag_name
            }
        };

        console.log(message_object);

        send_message_object(message_object);
    },

    // collects the general-settings checkboxes and sends them as one save_server_settings message
    save_server_settings_button_onclick: function(event)
    {
        event.stopPropagation();

        let message_object = {
            message: {
                type: "save_server_settings",
                display_country_flags: document.getElementById("server-settings-general-display-flags-checkbox").checked,
                enable_audio: document.getElementById("server-settings-general-enable-audio").checked,
                enable_music_bot_audio: document.getElementById("server-settings-general-enable-music-bot-audio-checkbox").checked,
                hide_clients_in_password_channels: document.getElementById("server-settings-general-hide-clients-in-password-protected-channels").checked,
                allow_temp_channels: document.getElementById("server-settings-general-allow-temp-channels-checkbox").checked,
                allow_typing_indicator: document.getElementById("server-settings-general-allow-typing-indicator-checkbox").checked,
                is_sending_text_to_idle_clients_allowed: document.getElementById("server-settings-general-allow-text-to-idle-clients-checkbox").checked,
                allow_private_messages: document.getElementById("server-settings-general-allow-private-messages-checkbox").checked,
                allow_file_uploads: document.getElementById("server-settings-general-allow-file-uploads-checkbox").checked,
                file_upload_max_size_mb: parseInt(document.getElementById("server-settings-general-file-upload-max-size-input").value) || 10,
                allow_chat_pictures: document.getElementById("server-settings-general-allow-pictures-checkbox").checked,
                chat_picture_max_size_mb: parseInt(document.getElementById("server-settings-general-picture-max-size-input").value) || 4,
                is_same_ip_address_allowed: document.getElementById("server-settings-general-allow-same-ip-checkbox").checked,
                minimum_rsa_key_bits: parseInt(document.getElementById("server-settings-general-minimum-rsa-bits-input").value) || 2048,
                announce_minimum_rsa_key_bits: document.getElementById("server-settings-general-announce-rsa-bits-checkbox").checked,
                is_country_blocking_active: document.getElementById("server-settings-general-country-blocking-checkbox").checked,
                blocked_countries: g_blocked_countries_draft
            }
        };

        send_message_object(message_object);
    },

    // saves only the four log toggles; save_server_settings applies just the fields present,
    // so the general-settings values are left untouched
    server_settings_log_save_button_onclick: function(event)
    {
        event.stopPropagation();

        let message_object = {
            message: {
                type: "save_server_settings",
                log_client_joins: document.getElementById("server-settings-log-joins-checkbox").checked,
                log_username_changes: document.getElementById("server-settings-log-renames-checkbox").checked,
                log_tag_changes: document.getElementById("server-settings-log-tags-checkbox").checked,
                log_server_settings_updates: document.getElementById("server-settings-log-settings-checkbox").checked,
                log_kicks_and_bans: document.getElementById("server-settings-log-kicks-bans-checkbox").checked,
                log_client_disconnects: document.getElementById("server-settings-log-disconnects-checkbox").checked,
                log_failed_attempts: document.getElementById("server-settings-log-failed-checkbox").checked,
                admin_log_max_size_mb: parseInt(document.getElementById("server-settings-log-max-size-input").value) || 10,
                admin_log_retention_days: parseInt(document.getElementById("server-settings-log-retention-select").value) || 7
            }
        };

        send_message_object(message_object);
    },

    server_settings_log_refresh_button_onclick: function(event)
    {
        event.stopPropagation();
        send_message_object({ message: { type: "admin_log_request" } });
    },

    // the server clears, stamps who did it, and replies with the fresh log on its own
    server_settings_log_clear_button_onclick: function(event)
    {
        event.stopPropagation();
        send_message_object({ message: { type: "admin_log_clear" } });
    },

    // becomes the given identity: asks the worker to derive the rsa keypair from the passphrase
    // and, when connected, drops the socket to reconnect as it. shared tail of the identity-file
    // and the raw-passphrase import paths - the caller has already validated the string
    identity_apply_passphrase: function(identity_passphrase_string)
    {
        // clears the identity slot; the driver dials again only once the new keypair exists
        request_identity(identity_passphrase_string);

        document.getElementById("identity-string-use-container").style.display = "none";
        document.getElementById("background-container").style.display = "none";
        document.getElementById("another-buttons-sub-loading-container").style.display = "block";
        document.getElementById("another-buttons-sub-container").style.display = "none";

        // invoked while connected (top bar "identity" button): drop the connection and,
        // once the socket is down and the new keypair exists, reconnect as that identity.
        // g_is_authenticated is the live "connected" signal - g_is_websocket_connected is a
        // dead flag that is never set true anywhere, do not gate on it
        if (g_is_authenticated == true)
        {
            // deliberate close; the user asked for this reconnect, so it is a button-class
            // request and redials once the new identity slot fills
            g_is_identity_switch_in_progress = true;
            g_websocket_worker.postMessage({ type: "close" });
            request_connect("button");
        }
    },

    // confirm on the raw-passphrase side of the import dialog: validates the 200-char passphrase
    identity_string_use_confirm_button: function()
    {
        let textarea = document.getElementById("identity-string-use-textarea");
        let identity_passphrase_string = textarea.value;

        if (identity_passphrase_string != null && identity_passphrase_string.length > 0)
        {

            if (identity_passphrase_string.length < 199)
            {
                custom_alert("[error] Cannot import identity. This is not identity passphrase. Identity passphrase is 200 characters long. For your own protection public/private keypair from this 'passphrase' will not be created");
                return;
            }

            textarea.value = "";
            UI.identity_apply_passphrase(identity_passphrase_string);
        }
    },

    // the exported identity file is named after the current username (fallback when there is none)
    identity_export_file_name: function()
    {
        let base_name = (typeof g_local_username === "string" && g_local_username.length > 0) ? g_local_username : "identity";

        base_name = base_name.replace(/[\\/:*?"<>|]/g, "_").trim();
        if (base_name.length == 0)
        {
            base_name = "identity";
        }

        return base_name + ".lmn";
    },

    // "save identity file": wraps the identity passphrase in a <username>.lmn download
    export_identity_file_button_onclick: function(event)
    {
        event.stopPropagation();

        if (identity_string == null || identity_string.length == 0)
        {
            custom_alert("no identity yet - it is still being generated, try again in a moment");
            return;
        }

        let file_name = UI.identity_export_file_name();

        // the android webview cannot save a blob url; java writes it into Downloads
        if (typeof Android !== "undefined" && Android != null && typeof Android.JavaExportSaveFile === "function")
        {
            Android.JavaExportSaveFile(file_name, "application/octet-stream", btoa(identity_string));
            return;
        }

        let url = URL.createObjectURL(new Blob([identity_string], { type: "application/octet-stream" }));
        let anchor = document.createElement("a");

        anchor.href = url;
        anchor.download = file_name;
        anchor.style.display = "none";
        document.body.appendChild(anchor);
        anchor.click();
        anchor.remove();

        setTimeout(function() { URL.revokeObjectURL(url); }, 60000);
    },

    choose_identity_file_button_onclick: function(event)
    {
        event.stopPropagation();
        document.getElementById("identity-file-input").click();
    },

    identity_file_input_onchange: function()
    {
        let input = document.getElementById("identity-file-input");

        if (input.files == null || input.files.length == 0)
        {
            return;
        }

        UI.identity_read_identity_file(input.files[0]);

        // so choosing the same file again still fires a change event
        input.value = "";
    },

    // reads a picked or dropped identity file; a valid one arms the confirm button
    identity_read_identity_file: function(file)
    {
        let status_line = document.getElementById("identity-file-status");
        let confirm_button = document.getElementById("identity-file-confirm-button");

        g_pending_identity_file_string = "";
        confirm_button.style.display = "none";

        if (file == null)
        {
            return;
        }

        // an identity file holds a 200-char passphrase; anything big is some other file
        if (file.size > 4096)
        {
            status_line.textContent = "'" + file.name + "' is not an identity file";
            return;
        }

        let reader = new FileReader();

        reader.onload = function()
        {
            // tolerate editors: strip a BOM and any whitespace or newlines around the passphrase
            let passphrase = String(reader.result).replace(/^﻿/, "").trim();

            // strict on purpose: a longer string would derive a DIFFERENT identity without any error
            if (passphrase.length < 199 || passphrase.length > 200)
            {
                status_line.textContent = "'" + file.name + "' does not contain an identity passphrase";
                return;
            }

            g_pending_identity_file_string = passphrase;
            status_line.textContent = "'" + file.name + "' looks good - confirm to connect as that identity";
            confirm_button.style.display = "inline-block";
        };

        reader.onerror = function()
        {
            status_line.textContent = "could not read '" + file.name + "'";
        };

        reader.readAsText(file);
    },

    identity_file_confirm_button_onclick: function(event)
    {
        event.stopPropagation();

        if (g_pending_identity_file_string.length == 0)
        {
            return;
        }

        let passphrase = g_pending_identity_file_string;
        g_pending_identity_file_string = "";
        UI.identity_apply_passphrase(passphrase);
    },

    // flips the import dialog between its file side and the raw-passphrase side
    identity_mode_toggle_onclick: function(event)
    {
        event.stopPropagation();

        let file_mode = document.getElementById("identity-file-mode");
        let string_mode = document.getElementById("identity-string-mode");
        let toggle_link = document.getElementById("identity-mode-toggle-link");
        let is_file_mode_active = file_mode.style.display != "none";

        file_mode.style.display = is_file_mode_active ? "none" : "block";
        string_mode.style.display = is_file_mode_active ? "block" : "none";
        toggle_link.textContent = is_file_mode_active ? "use an identity file instead" : "use an identity passphrase instead";
    },

    // font-size slider: stores g_selected_font_size and applies it to the chat input
    chat_input_font_size_range_oninput: function(event)
    {
        g_selected_font_size = parseInt(event.srcElement.value);
        document.getElementById("chat-input-container-text-input").style.fontSize = event.srcElement.value + "px";
    },

    // color picker: stores g_selected_font_color and paints the swatch and chat input with it
    color_picker_oninput: function(event)
    {
        g_selected_font_color = event.srcElement.value;
        document.getElementById("color-picker-selector-div").style.backgroundColor = event.srcElement.value;
        document.getElementById("chat-input-container-text-input").style.color = event.srcElement.value;
    },

    // opens the dialog that reveals the local identity (export button + raw passphrase)
    show_secret_identity_string_onclick: function(event)
    {
        event.stopPropagation();
        document.getElementById("secret-identity-string-container").style.display = "block";
        document.getElementById("background-container").style.display = "block";
        document.getElementById("p-secret-identity-string").innerHTML = identity_string;
        document.getElementById("export-identity-file-name").textContent = "saves as " + UI.identity_export_file_name();
    },

    // toggles the server settings window (admins only): on open it is re-centered by measured
    // pixels and the icons tab is marked selected; tracked in g_is_server_settings_tab_visible
    enter_server_settings_onclick: function(event)
    {
        event.stopPropagation();

        console.log("enter_server_settings_onclick");

        // the button is always visible now, but server settings are admin-only: a
        // non-admin click does nothing (the server would reject the requests anyway)
        if (is_local_client_admin() == false)
        {
            return;
        }

        if (g_is_server_settings_tab_visible == false)
        {
            // center it on every open (clearing any position left over from a previous drag),
            // by measured pixels - the same scheme the popup dialogs use
            let tab = document.getElementById("server-settings-tab");
            tab.style.position = "fixed";
            tab.style.margin = "0";
            tab.style.transform = "none";
            tab.style.display = "block";
            let rect = tab.getBoundingClientRect();
            tab.style.left = Math.max(0, Math.round((window.innerWidth - rect.width) / 2)) + "px";
            tab.style.top = Math.max(0, Math.round((window.innerHeight - rect.height) / 2)) + "px";
            g_is_server_settings_tab_visible = true;

            // it always opens on the g_icons tab, so mark that one as selected
            let nav_items = document.getElementsByClassName("server-settings-tab-li");
            for (let i = 0; i < nav_items.length; i++)
            {
                nav_items[i].classList.toggle("server-settings-tab-li-selected", nav_items[i].getAttribute("data-action") == "0");
            }
        }
        else
        {
            document.getElementById("server-settings-tab").style.display = "none";
            g_is_server_settings_tab_visible = false;
        }
    },

    // hides the server settings window and clears its visibility flag
    close_button_server_settings_onclick: function()
    {
        document.getElementById("server-settings-tab").style.display = "none";
        g_is_server_settings_tab_visible = false;
    },

    // opens the import-identity dialog and its background overlay
    import_identity_button_onclick: function()
    {
        // fresh walk-through every time: file side first, nothing picked yet
        g_pending_identity_file_string = "";
        document.getElementById("identity-file-status").textContent = "";
        document.getElementById("identity-file-confirm-button").style.display = "none";
        document.getElementById("identity-file-mode").style.display = "block";
        document.getElementById("identity-string-mode").style.display = "none";
        document.getElementById("identity-mode-toggle-link").textContent = "use an identity passphrase instead";

        document.getElementById("identity-string-use-container").style.display = "block";
        document.getElementById("background-container").style.display = "block";
    },

    // toggles g_show_hide_toggle and hides/shows every country flag in the user list
    hide_show_flags_button_onclick: function()
    {
        let elements_count1 = document.getElementsByClassName("connected-client-country-flag").length;

        if (g_show_hide_toggle == false)
        {
            for (let i = 0; i < elements_count1; i++)
            {
                document.getElementsByClassName("connected-client-country-flag")[i].classList.add("connected-client-country-flag-hide");
            }
        }
        else
        {
            for (let i = 0; i < elements_count1; i++)
            {
                document.getElementsByClassName("connected-client-country-flag")[i].classList.remove("connected-client-country-flag-hide");
            }
        }

        g_show_hide_toggle = !g_show_hide_toggle;
    },

    // right click on the chat input: opens the font-selection context menu above the cursor
    chat_input_container_on_mousedown: function(event)
    {

        if (event.which == 1)
        {

        }

        else if (event.which == 3)
        {
            event.stopPropagation();
            UI.delete_contextmenus();
            console.log("test");

            let contextmenu_html = "<div class=\"chat-input-container-context-menu\" style=\"top: " + (event.clientY - 130).toString() + "px; left:" + event.clientX.toString() + "px; \">\n\
                                                <div class='chat-input-container-context-menu-background'>\n\
                                                </div>\n\
                                                <div class='chat-input-container-context-menu-items'>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-default'>font: default</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Yellowtail'>font: Yellowtail</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Bruno-Ace-SC'>font: Bruno Ace</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Raleway'>font: Raleway</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Kavoon'>font: Kavoon</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Reenie-Beanie'>font: Reenie Beanie</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Sacramento'>font: Sacramento</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Sofia'>font: Sofia</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Nova-Mono'>font: Nova Mono</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Codystar'>font: Codystar</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Londrina-Outline'>font: Londrina Outline</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Mystery-Quest'>font: Mystery Quest</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Nosifer'>font: Nosifer</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Simonetta'>font: Simonetta</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Hanalei'>font: Hanalei</p>\n\
                                                    <p class='context-menu-item' data-action='custom-font-usage-Just-Me-Again-Down-Here'>font: Just me</p>\n\
                                                </div>\n\
                                            </div>";

            document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", contextmenu_html);

            let contextmenu_item_count = document.getElementsByClassName("context-menu-item").length;

            for (let i = 0; i < contextmenu_item_count; i++)
            {
                document.getElementsByClassName("context-menu-item")[i].addEventListener("click", UI.contextmenuitem_chat_input_container_onclick);
            }
        }
    },

    // send in the poke dialog: sends poke_client with the typed message to selected_client_id
    // (an empty message does nothing) and closes the dialog
    poke_client_send_button_onclick: function(event)
    {
        event.stopPropagation();

        let poke_message = document.getElementById("input-poke-client").value;

        if (poke_message.length == 0)
        {
            return;
        }

        document.getElementById("poke-client-enter-container").style.display = "none";

        let message_object = {
            message:
            {
                type: "poke_client",
                client_id: parseInt(selected_client_id),
                poke_message: poke_message
            }
        };

        send_message_object(message_object);
    },

    // send in the set-alias dialog: checks the alias is not held by another online or stored
    // client (case-insensitive), then sends set_alias_request; an empty value clears the alias
    set_alias_send_button_onclick: function(event)
    {
        event.stopPropagation();

        // empty value is valid - it clears the registered alias
        let alias_value = document.getElementById("input-set-alias").value;

        // aliases must stay unique (they are the handle offline entries are paired by). the
        // server enforces this and would silently drop a clash, so check here for the feedback.
        // compared case-insensitively, matching the server's rule
        if (alias_value.length > 0)
        {
            let wanted_alias = alias_value.toLowerCase();
            let is_taken = false;

            // the target's own alias is not a clash - re-submitting it is a no-op, and its
            // stored entry would otherwise look like somebody else holding the name
            let target_client = get_client_by_client_id(selected_client_id);
            let target_alias = (target_client != null && typeof target_client.alias === "string") ? target_client.alias.toLowerCase() : "";

            for (let i = 0; i < g_client_list.length; i++)
            {
                if (g_client_list[i].client_id != parseInt(selected_client_id) && typeof g_client_list[i].alias === "string" && g_client_list[i].alias.toLowerCase() == wanted_alias)
                {
                    is_taken = true;
                    break;
                }
            }

            for (let i = 0; i < g_offline_client_list.length && is_taken == false; i++)
            {
                if (g_offline_client_list[i].alias.toLowerCase() == wanted_alias && g_offline_client_list[i].alias.toLowerCase() != target_alias)
                {
                    is_taken = true;
                }
            }

            if (is_taken == true)
            {
                custom_alert("alias '" + alias_value + "' is already taken");
                return;
            }
        }

        document.getElementById("set-alias-enter-container").style.display = "none";

        let message_object = {
            message:
            {
                type: "set_alias_request",
                client_id: parseInt(selected_client_id),
                alias: alias_value
            }
        };

        send_message_object(message_object);
    },

    // closes the set-alias dialog
    close_button_set_alias_onclick: function(event)
    {
        event.stopPropagation();
        document.getElementById("set-alias-enter-container").style.display = "none";
    },

    // toggles g_are_sound_effects_enabled, swaps the button icon, tells the android wrapper
    // (when present) and refreshes the toolbar state classes
    sound_effects_button_onclick: function()
    {
        g_are_sound_effects_enabled = !g_are_sound_effects_enabled;

        if (g_are_sound_effects_enabled == true)
        {
            document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGc+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxMSwzMjAgMzcwLDMxOCAzNDAsMzMyQzI5NywzNTQgMzAxLDM2MCAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMCwyNTYgMTQ5LDE2MkMxNTMsMTU5IDE5MSwxMzYgMTk5LDEzNEMyMDMsMTMyIDI0NiwxMTggMjQ5LDExOEMyNjgsMTE3IDI2NywxMTMgMjg5LDExM0MyOTEsMTEzIDI5MCwxMTEgMjkyLDExMUMzMTEsMTA5IDM3MSwxMDkgMzc1LDExMUMzODEsMTE0IDM4NSwxMTIgMzg3LDExM0MzOTQsMTE3IDM5NSwxMTUgMzk1LDExNUM0MDIsMTE4IDQwMiwxMTYgNDAyLDExN0M0MTEsMTIxIDQzOCwxMjQgNDQyLDEzNFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIxMjAuOTE0MDA0LDIxNTAuNTUzNjA1KSI+PHBhdGggZD0iTTQ0NSwxMzJDNDQ3LDEzMCA0NDcsMTI5IDQ0OSwxMzFDNDU0LDEzMyA0NTUsMTMxIDQ1OCwxMzJDNDY4LDEzNyA1ODIsMTU0IDU4MiwyNjNDNTgyLDI2NSA1ODAsMjY1IDU4MCwyNjdDNTc5LDMyMSA1NTQsMzIxIDU0NywzMDVDNTQyLDI5NiA1NTgsMjYxIDU0NywyMjhDNTM2LDE5NiA0OTksMTc1IDUwOCwxODRDNTE2LDE5MyA1MTUsMTk0IDUyMiwyMDRDNTM2LDIyNSA1NDIsMjYxIDU0MiwyNjNDNTQyLDI4OSA1NDEsMjg4IDU0MCwzMDZDNTQwLDMwOSA1MzksMzA4IDUzOSwzMTFDNTM4LDMxNiA1MzgsMzE1IDUzNywzMjFDNTM2LDMyOCA1MzUsMzI3IDUzMywzMzRDNTMzLDMzOSA1MjksMzUwIDUyOCwzNTNDNTI2LDM2NCA1MjEsMzY1IDUyNywzNzVDNTM3LDM4OSA2MDUsNDMyIDU0OCw0NjJDNTQxLDQ2NSA1NDIsNDY2IDU0NSw0NzNDNTU0LDQ5MSA1MzUsNDk1IDUzNyw0OTlDNTUyLDUxOCA1MzAsNTI0IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjkxLDQwNCAzMzgsMzQ1IDM5MywzNDVDNDMxLDM0NSA0NDEsMzMyIDQ1NiwzMjBDNDU5LDMxNyA0NTksMzE3IDQ2MiwzMTRDNDY0LDMxMiA0NzYsMjk5IDQ4MywyODZDNTA0LDI0MyA0OTgsMjM5IDUwMSwyMzRDNTA0LDIyNiA1MDIsMjIzIDUwMiwyMjFDNTA2LDIxMiA1MDEsMTk0IDUwNSwxODNDNTA3LDE3OCA0NDcsMTM3IDQ0NSwxMzJaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik04NDMsNDcwQzg0NCw0ODMgODQ1LDQ4MiA4NDUsNDk0Qzg0NSw1MjcgODQ1LDUyNyA4NDIsNTQ2QzgzMyw1OTAgODMxLDU4OSA4MTIsNjI5QzgxMiw2MzAgNzk5LDY0OSA3OTgsNjUwQzc5OCw2NTEgNzc4LDY3NyA3NzAsNjgyQzc2MSw2ODkgNzM0LDY3NyA3NTYsNjU1QzgwNSw2MDYgODQ0LDUwMiA3OTAsNDAxQzc2MSwzNDYgNzMzLDM0NiA3NTUsMzI3Qzc2NywzMTYgNzgxLDMzOCA3OTIsMzUwQzgwMSwzNTkgODM0LDQxNSA4MzcsNDM4QzgzNyw0NDIgODM5LDQ0MiA4NDAsNDUzQzg0MCw0NTQgODQzLDQ2MiA4NDMsNDcwWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNzI2LDQ3MUM3MjUsNDY4IDcyMCw0NTIgNzE4LDQ1MEM2OTcsNDA3IDY3NCw0MDYgNjk0LDM4OEM3MDIsMzgxIDcxNSwzODkgNzE4LDM5NkM3MjEsNDAyIDczMiw0MDkgNzQ0LDQzNkM3NjMsNDc0IDc2MCw1MTMgNzYwLDUxM0M3NTgsNTE5IDc1Nyw1MzUgNzU3LDUzOEM3NTIsNTYyIDczMiw1OTcgNzI1LDYwNEM3MTUsNjE1IDcxNyw2MTggNzA0LDYyNEM2OTksNjI2IDY4OCw2MTcgNjg4LDYxM0M2ODQsNTkzIDcwNiw1OTQgNzIxLDU1M0M3MjUsNTQwIDcyNSw1NDAgNzI4LDUyN0M3MzMsNDk4IDcyNiw0NzggNzI2LDQ3MVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTY1NSw1MTZDNjU1LDUxNCA2NTcsNTA2IDY1NCw0OTFDNjUwLDQ2NyA2MTgsNDUyIDY0Niw0MzdDNjU1LDQzMyA2NzEsNDU0IDY3NSw0NjJDNzA4LDUyNSA2NTcsNTgzIDY0Myw1NjlDNjIwLDU0OCA2NTEsNTQ3IDY1NSw1MTZaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNODQzLDQ3MEM4NDQsNDgzIDg0NSw0ODIgODQ1LDQ5NEM4NDUsNTI3IDg0NSw1MjcgODQyLDU0NkM4MzMsNTkwIDgzMSw1ODkgODEyLDYyOUM4MTIsNjMwIDc5OSw2NDkgNzk4LDY1MEM3OTgsNjUxIDc3OCw2NzcgNzcwLDY4MkM3NjEsNjg5IDczNCw2NzcgNzU2LDY1NUM4MDUsNjA2IDg0NCw1MDIgNzkwLDQwMUM3NjEsMzQ2IDczMywzNDYgNzU1LDMyN0M3NjcsMzE2IDc4MSwzMzggNzkyLDM1MEM4MDEsMzU5IDgzNCw0MTUgODM3LDQzOEM4MzcsNDQyIDgzOSw0NDIgODQwLDQ1M0M4NDAsNDU0IDg0Myw0NjIgODQzLDQ3MFoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTcyNiw0NzFDNzI1LDQ2OCA3MjAsNDUyIDcxOCw0NTBDNjk3LDQwNyA2NzQsNDA2IDY5NCwzODhDNzAyLDM4MSA3MTUsMzg5IDcxOCwzOTZDNzIxLDQwMiA3MzIsNDA5IDc0NCw0MzZDNzYzLDQ3NCA3NjAsNTEzIDc2MCw1MTNDNzU4LDUxOSA3NTcsNTM1IDc1Nyw1MzhDNzUyLDU2MiA3MzIsNTk3IDcyNSw2MDRDNzE1LDYxNSA3MTcsNjE4IDcwNCw2MjRDNjk5LDYyNiA2ODgsNjE3IDY4OCw2MTNDNjg0LDU5MyA3MDYsNTk0IDcyMSw1NTNDNzI1LDU0MCA3MjUsNTQwIDcyOCw1MjdDNzMzLDQ5OCA3MjYsNDc4IDcyNiw0NzFaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik02NTUsNTE2QzY1NSw1MTQgNjU3LDUwNiA2NTQsNDkxQzY1MCw0NjcgNjE4LDQ1MiA2NDYsNDM3QzY1NSw0MzMgNjcxLDQ1NCA2NzUsNDYyQzcwOCw1MjUgNjU3LDU4MyA2NDMsNTY5QzYyMCw1NDggNjUxLDU0NyA2NTUsNTE2WiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjwvZz48L3N2Zz4=)";
        }
        else
        {
            document.getElementById("sound-effects-button").style.backgroundImage = "url(data:image/svg+xml;base64,PD94bWwgdmVyc2lvbj0iMS4wIiBlbmNvZGluZz0iVVRGLTgiIHN0YW5kYWxvbmU9Im5vIj8+PCFET0NUWVBFIHN2ZyBQVUJMSUMgIi0vL1czQy8vRFREIFNWRyAxLjEvL0VOIiAiaHR0cDovL3d3dy53My5vcmcvR3JhcGhpY3MvU1ZHLzEuMS9EVEQvc3ZnMTEuZHRkIj48c3ZnIHdpZHRoPSIxODJweCIgaGVpZ2h0PSIxNjBweCIgdmVyc2lvbj0iMS4xIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hsaW5rIiB4bWw6c3BhY2U9InByZXNlcnZlIiB4bWxuczpzZXJpZj0iaHR0cDovL3d3dy5zZXJpZi5jb20vIiBzdHlsZT0iZmlsbC1ydWxlOmV2ZW5vZGQ7Y2xpcC1ydWxlOmV2ZW5vZGQ7c3Ryb2tlLWxpbmVqb2luOnJvdW5kO3N0cm9rZS1taXRlcmxpbWl0OjI7Ij48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjI0LDAsMCwwLjI0LC01MTguMDg4MjEyLC01NDIuNDU0NDM4KSI+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik0zMzEsMzM3QzI5NywzNTUgMjk4LDM2MyAyNjIsMzg5QzI1NiwzOTMgMjU1LDM5MyAyNDcsMzg2QzI0MSwzNzkgMjEzLDM2OCAyMDcsMzY4QzIwMSwzNjcgMjAyLDM2OCAxOTUsMzY5QzE0MCwzNzUgMTU2LDQ4NiAyMDEsNDkyQzIyNSw0OTUgMjIyLDUwNiAyMjEsNTQ1QzIyMSw1NDggMjIwLDU0NyAyMjAsNTUwQzIxOSw1NzUgMjE3LDU3NCAyMTYsNTk5QzIxNiw2MDEgMjE1LDYwMiAyMTUsNjA1QzIxMiw2NzkgMjI3LDcxNCAyNTAsNzQ5QzI1Nyw3NTkgMjM5LDc1OCAyMzYsNzU3QzIyNyw3NTMgMTkzLDc1OCAxNDEsNzQwQzEzMiw3MzcgMTMyLDczNiAxMjQsNzMzQzg1LDcyMCA2MSw2ODUgNTcsNjc3QzQyLDY0NiA0Niw2MjcgNDUsNjIzQzQxLDYxMSA0NCw2MDcgNDMsNjAyQzM5LDU5MSA0MSw1MzcgMzgsNTI1QzM4LDUyNSAzOCw0NzkgMzgsNDc5QzQzLDQ1NiAzNyw0MjkgNDAsNDI0QzQ1LDQxNCAzMSwyNjYgMTM5LDE3MUwzMzEsMzM3Wk0xNzksMTQ0QzE4OCwxMzkgMTk1LDEzNSAxOTksMTM0QzIwMywxMzIgMjQ2LDExOCAyNDksMTE4QzI2OCwxMTcgMjY3LDExMyAyODksMTEzQzI5MSwxMTMgMjkwLDExMSAyOTIsMTExQzMxMSwxMDkgMzcxLDEwOSAzNzUsMTExQzM4MSwxMTQgMzg1LDExMiAzODcsMTEzQzM5NCwxMTcgMzk1LDExNSAzOTUsMTE1QzQwMiwxMTggNDAyLDExNiA0MDIsMTE3QzQxMSwxMjEgNDM4LDEyNCA0NDIsMTM0QzQ0MywxMzUgNDQzLDEzNSA0NDksMTQwQzQ1NCwxNDYgNDUzLDE0NiA0NTksMTUyQzQ3MywxNjYgNDcyLDE3MyA0NzUsMTgyQzQ3NiwxODggNDg1LDIyMyA0NjgsMjU4QzQ2NSwyNjMgNDQ2LDMwMiA0MTcsMzE3QzQxNCwzMTggNDAwLDMxOCAzODMsMzIxTDE3OSwxNDRaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMTIwLjkxNDAwNCwyMTUwLjU1MzYwNSkiPjxwYXRoIGQ9Ik00MTAsMzQ0QzQzNCwzNDEgNDQzLDMzMCA0NTYsMzIwQzQ1OSwzMTcgNDU5LDMxNyA0NjIsMzE0QzQ2NCwzMTIgNDc2LDI5OSA0ODMsMjg2QzUwNCwyNDMgNDk4LDIzOSA1MDEsMjM0QzUwNCwyMjYgNTAyLDIyMyA1MDIsMjIxQzUwNiwyMTIgNTAxLDE5NCA1MDUsMTgzQzUwNywxNzggNDQ3LDEzNyA0NDUsMTMyQzQ0NywxMzAgNDQ3LDEyOSA0NDksMTMxQzQ1NCwxMzMgNDU1LDEzMSA0NTgsMTMyQzQ2OCwxMzcgNTgyLDE1NCA1ODIsMjYzQzU4MiwyNjUgNTgwLDI2NSA1ODAsMjY3QzU3OSwzMjEgNTU0LDMyMSA1NDcsMzA1QzU0MiwyOTYgNTU4LDI2MSA1NDcsMjI4QzUzNiwxOTYgNDk5LDE3NSA1MDgsMTg0QzUxNiwxOTMgNTE1LDE5NCA1MjIsMjA0QzUzNiwyMjUgNTQyLDI2MSA1NDIsMjYzQzU0MiwyODkgNTQxLDI4OCA1NDAsMzA2QzU0MCwzMDkgNTM5LDMwOCA1MzksMzExQzUzOCwzMTYgNTM4LDMxNSA1MzcsMzIxQzUzNiwzMjggNTM1LDMyNyA1MzMsMzM0QzUzMywzMzkgNTI5LDM1MCA1MjgsMzUzQzUyNiwzNjQgNTIxLDM2NSA1MjcsMzc1QzUzNywzODkgNjA1LDQzMiA1NDgsNDYyQzU0Nyw0NjIgNTQ3LDQ2MiA1NDYsNDYyTDQxMCwzNDRaTTU0MCw1MTlDNTM2LDUyNCA1MzEsNTI3IDUzMiw1MzJDNTM1LDU0MyA1MzMsNTYyIDUzMSw1NjRDNTIzLDU4MCA1MjEsNTg1IDUwNCw1OTNDNDk1LDU5OCA0NjMsNTkxIDQ2Myw1OTFDNDQyLDU4MSA0MjcsNTkwIDQyNCw1OTJDNDEyLDU5OSA0MTUsNjAyIDM5MSw2NzVDMzgyLDcwNSAzODMsNzA2IDM4Miw3MDhDMzc5LDcxNSAzODIsNzE3IDM4Myw3MTlDMzgzLDczMiA0MDEsNzQ3IDQwNSw3NTJDNDE3LDc2NCA0MDAsNzczIDM5OSw3NzRDMzkzLDc3NyAyOTcsNzYzIDI5Myw3NjJDMjkwLDc2MiAyNTUsNzA3IDI1NSw3MDVDMjU0LDcwNSAyNTQsNzAyIDI1MSw2OTRDMjI5LDY0MSAyNDcsNTY4IDI0Nyw1MjBDMjQ3LDUwNCAyNDYsNTA1IDI0Miw0ODlDMjQxLDQ4MiAyMjcsNDcyIDIyNSw0NzJDMjEzLDQ2OSAyMTQsNDY4IDIxMiw0NjhDMTg3LDQ2NiAxODIsNDI1IDE4Nyw0MTBDMTk5LDM3MiAyMzQsNDEwIDIzOCw0MTJDMjU4LDQyMyAyNjYsNDE2IDI3Myw0MTNDMjg2LDQwNyAzMTUsMzczIDM1MiwzNTZMNTQwLDUxOVoiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgxLDAsMCwxLDIwNjguMjg4MzgzLDIxMTUuNzEwNDc5KSI+PHBhdGggZD0iTTg0Myw0NzBDODQ0LDQ4MyA4NDUsNDgyIDg0NSw0OTRDODQ1LDUyNyA4NDUsNTI3IDg0Miw1NDZDODMzLDU5MCA4MzEsNTg5IDgxMiw2MjlDODEyLDYzMCA3OTksNjQ5IDc5OCw2NTBDNzk4LDY1MSA3NzgsNjc3IDc3MCw2ODJDNzYxLDY4OSA3MzQsNjc3IDc1Niw2NTVDODA1LDYwNiA4NDQsNTAyIDc5MCw0MDFDNzYxLDM0NiA3MzMsMzQ2IDc1NSwzMjdDNzY3LDMxNiA3ODEsMzM4IDc5MiwzNTBDODAxLDM1OSA4MzQsNDE1IDgzNyw0MzhDODM3LDQ0MiA4MzksNDQyIDg0MCw0NTNDODQwLDQ1NCA4NDMsNDYyIDg0Myw0NzBaIiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PGcgdHJhbnNmb3JtPSJtYXRyaXgoMSwwLDAsMSwyMDY4LjI4ODM4MywyMTE1LjcxMDQ3OSkiPjxwYXRoIGQ9Ik03MjYsNDcxQzcyNSw0NjggNzIwLDQ1MiA3MTgsNDUwQzY5Nyw0MDcgNjc0LDQwNiA2OTQsMzg4QzcwMiwzODEgNzE1LDM4OSA3MTgsMzk2QzcyMSw0MDIgNzMyLDQwOSA3NDQsNDM2Qzc2Myw0NzQgNzYwLDUxMyA3NjAsNTEzQzc1OCw1MTkgNzU3LDUzNSA3NTcsNTM4Qzc1Miw1NjIgNzMyLDU5NyA3MjUsNjA0QzcxNSw2MTUgNzE3LDYxOCA3MDQsNjI0QzY5OSw2MjYgNjg4LDYxNyA2ODgsNjEzQzY4NCw1OTMgNzA2LDU5NCA3MjEsNTUzQzcyNSw1NDAgNzI1LDU0MCA3MjgsNTI3QzczMyw0OTggNzI2LDQ3OCA3MjYsNDcxWiIgc3R5bGU9ImZpbGw6d2hpdGU7Ii8+PC9nPjxnIHRyYW5zZm9ybT0ibWF0cml4KDEsMCwwLDEsMjA2OC4yODgzODMsMjExNS43MTA0NzkpIj48cGF0aCBkPSJNNjU1LDUxNkM2NTUsNTE0IDY1Nyw1MDYgNjU0LDQ5MUM2NTAsNDY3IDYxOCw0NTIgNjQ2LDQzN0M2NTUsNDMzIDY3MSw0NTQgNjc1LDQ2MkM3MDgsNTI1IDY1Nyw1ODMgNjQzLDU2OUM2MjAsNTQ4IDY1MSw1NDcgNjU1LDUxNloiIHN0eWxlPSJmaWxsOndoaXRlOyIvPjwvZz48ZyB0cmFuc2Zvcm09Im1hdHJpeCgwLjY1ODc0OCwwLjU3MjE0LC0wLjY1NTczMiwwLjc1NDk5NCwyODczLjU3MjI1NywtMTEyMy4zODAwNykiPjxyZWN0IHg9IjE5NTIiIHk9IjMwMDMiIHdpZHRoPSIxMDk4IiBoZWlnaHQ9IjQ3IiBzdHlsZT0iZmlsbDp3aGl0ZTsiLz48L2c+PC9nPjwvc3ZnPg==)";
        }

        if (typeof Android !== 'undefined')
        {
            Android.JavaExportSetSoundEffectsStatus(g_are_sound_effects_enabled);
        }

        // authoritative mute at the Audio-object level: the per-call-site
        // g_are_sound_effects_enabled checks missed at least one play(), so gate the
        // sound itself - a leaked play() on a muted element makes no noise
        apply_sound_effects_muted();

        sync_toolbar_state_classes();
    },

    // toggles always-on microphone (g_is_microphone_always_on): on activates the mic, off
    // restores the push-to-talk button and stops sending audio; also told to the android wrapper
    activate_continous_audio_broadcast_onclick: function()
    {
        set_microphone_always_on(!g_is_microphone_always_on);
        UI.activate_continous_audio_broadcast_apply();
    },

    // everything the toggle DOES once the flag is set: the android bridge, the microphone and
    // the repaint. split out of the handler above so the android settings push can apply the
    // same effects after setting the flag itself - going back through the click handler would
    // toggle the flag a second time and invert it
    activate_continous_audio_broadcast_apply: function()
    {
        if (g_is_running_in_android_webview)
        {
            Android.JavaExportSetContinousAudioBroadcastStatus(g_is_microphone_always_on);
        }

        if (g_is_microphone_always_on)
        {
            // on speakers an always-open mic picks the others up and sends it back, so they hear themselves
            custom_alert("use headphones with continuous transmission, otherwise everyone else hears their own echo");
            activate_microphone();
        }
        else
        {
            document.getElementById("microphone-always-broadcasting-audio-button").style.backgroundImage = "url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAYAAABXAvmHAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAIySURBVGhD7ZnBTsMwDIbbSZPQxAmJMXhGXoQnhcGBE0hcALtxqyWNndR2OpD2XZI0ifP/iddWa9858wNQNUsPUNUFt2Al4SleRlyCLBU/4mFiQ6WFayo17KhUY94B7e6PWE/BdAJW8YgQ45b6xFPySCETzAncgfZXrED5AQVr4qwGBPEvVB+QTJjyDwKrU6hW/Cm5OWc5AaX4PVUjVjfAiN8XxD9A8RZaMasaEMQfqT6DxD+H1pzVDLQQj6xioJV4pLmBluKRpgZaizcDQlhoSAqKZ4H++zDMly3FH6BrA3RpBnWn4H2eBftxEDUHoLnFaxK5I46gQBFjakh9CaWH1AGK44J4E64GmMXEnEe4eAgTc8LtR8wsVBRvxWQAND9ReRbxiHg8SOFY8RX3M1QjxJxPGeMV1spiTSGzeCveD7JVxSOeBlYXjxQNpDnI5KRaPBNvQOobqToBiPNIpbf46UkLMWY/4BqKDgtYxN9A8R5aXQ9xvqk+wWxYhMWARfwVFF+hpbt9jmgNuOV8TjxSa0CD2ysxTZlB3U3AnWfB/jBMhoZnoSHVLDmmRWlzmgK1wjRpUztBnfO1aMQjNZP+rHik9CATxcO6B8viMHVjmY9Ik4vioYje92F8s1xfyvBxgQP6i3cbGjpBl93J7oS0YG7nc6QxWu06F3QH6+NHhYiceMlsDVZj0uTIRCreKjxFa6Q0aTABsfHjwvT/vLf4EY0Jlet/bwDxNqERf+GCma77BRNIJZ4nCQCWAAAAAElFTkSuQmCC)";
            g_is_microphone_available = true;
            update_microphone_button();
            process_stop_sending_audio();
        }

        sync_toolbar_state_classes();
    },

    // send in the set-username dialog: requests the rename for selected_client_id and, when
    // that is the local client, updates g_client_list/g_local_username and the inline input now
    set_new_username_send_button_onclick: function(event)
    {
        event.stopPropagation();

        let new_username = document.getElementById("input-set-new-username").value;

        if (new_username.length == 0)
        {
            return;
        }

        client_msg.send_change_client_username_request(new_username, parseInt(selected_client_id));

        // client objects have no is_local_client field, so this never fired (ported from the fix repo)
        if (parseInt(selected_client_id) == local_client_id)
        {
            let index = get_client_index_in_array_by_client_id(local_client_id);
            g_client_list[index].username = new_username;
            g_local_username = new_username;
            { let rename_input = document.getElementById('connected-local-client-input'); if (rename_input != null) { rename_input.setAttribute('value', g_local_username); } }
        }

        document.getElementById("set-new-username-enter-container").style.display = "none";
    },

    // closes the poke dialog
    close_button_poke_client_onclick: function(event)
    {
        event.stopPropagation();
        document.getElementById("poke-client-enter-container").style.display = "none";
    },

    // closes the client info popup
    close_button_client_info_onclick: function(event)
    {
        event.stopPropagation();
        document.getElementById("client-info-container").style.display = "none";
    },

    // rebuilds the server-settings ban list from the given array: one row per ban with ip,
    // country, identity and a remove button (a "no bans" placeholder when empty)
    render_bans_list: function(bans)
    {
        let container = document.getElementById("server-settings-bans-list");
        if (container == null)
        {
            return;
        }

        container.innerHTML = "";

        if (bans == null || bans.length == 0)
        {
            let empty = document.createElement("p");
            empty.className = "ban-entry-empty";
            empty.innerText = "no bans";
            container.appendChild(empty);
            return;
        }

        for (let i = 0; i < bans.length; i++)
        {
            let ban = bans[i];

            let row = document.createElement("div");
            row.className = "ban-entry";

            let info = document.createElement("div");
            info.className = "ban-entry-info";

            let ip_span = document.createElement("span");
            ip_span.className = "ban-entry-ip";
            ip_span.innerText = ban.ip_address;

            let meta_span = document.createElement("span");
            meta_span.className = "ban-entry-meta";
            meta_span.innerText = (ban.country_iso_code != null && ban.country_iso_code.length > 0) ? ban.country_iso_code : "??";

            let identity_span = document.createElement("span");
            identity_span.className = "ban-entry-identity";
            identity_span.innerText = (ban.identity != null) ? ban.identity : "";

            info.appendChild(ip_span);
            info.appendChild(meta_span);
            info.appendChild(identity_span);

            let remove_button = document.createElement("div");
            remove_button.className = "ban-entry-remove";
            remove_button.innerText = "remove";
            remove_button.setAttribute("data-ban-ip", ban.ip_address);
            remove_button.onclick = UI.ban_remove_onclick;

            row.appendChild(info);
            row.appendChild(remove_button);
            container.appendChild(row);
        }
    },

    // remove button on a ban row: sends remove_ban for that ip and optimistically drops the row
    ban_remove_onclick: function(event)
    {
        event.stopPropagation();

        let ip = event.currentTarget.getAttribute("data-ban-ip");
        if (ip == null)
        {
            return;
        }

        let message_object = {
            message: {
                type: "remove_ban",
                ip_address: ip
            }
        };

        send_message_object(message_object);

        // optimistically remove the row; reopening settings re-fetches the authoritative list
        let row = event.currentTarget.parentNode;
        if (row != null && row.parentNode != null)
        {
            row.parentNode.removeChild(row);
        }
    },

    // closes the set-username dialog
    close_button_set_new_username_onclick: function(event)
    {
        event.stopPropagation();
        document.getElementById("set-new-username-enter-container").style.display = "none";
    },

    // stop-song button: restores the upload button and asks the server to stop the song stream
    stop_song_onclick: function(event)
    {
        event.stopPropagation();

        document.getElementById("custom-file-upload-button-song").style.display = "inline-block";
        document.getElementById("stop-song").style.display = "none";

        let message_object1 = {
            message:
            {
                type: "stop_song_stream"
            }
        };

        send_message_object(message_object1);
    },

    // clicking a toast hides it (and suppresses the link default)
    a_toast_onclick: function(event)
    {
        event.preventDefault();
        event.target.style.visibility = "hidden";
    },

    // closes the music bot management dialog
    musicbot_management_close_button_onclick: function(event)
    {
        event.stopPropagation();
        document.getElementById("musicbot-management-background-container").style.display = "none";
    },

    // upload in the music bot dialog: size-checks the mp3 (10mb), reads it, strips the data-url
    // prefix and streams it to the selected bot in 400-char parts via send_file_by_parts
    musicbot_management_confirm_upload_button_onclick: function(event)
    {
        event.stopPropagation();
        // document.getElementById("musicbot-management-background-container").style.display = "none";

        let fileInput = document.getElementById('musicbot-management-background-container-file-input');
        let files = fileInput.files;

        for (let x = 0; x < files.length; x++)
        {
            if (x == 0)
            {
                let file = files[x];
                let max_size = 10 * 1024 * 1024;

                if (file.size > max_size)
                {
                    custom_alert("mp3 is too large. Max size: 10mb");
                    return;
                }

                let reader = new FileReader();
                reader.onload = function (event)
                {
                    // use a LOCAL for the song bytes - reusing base64_picture_string_to_send left
                    // the song sitting in the chat picture buffer, so the next text message saw a
                    // non-empty picture and re-uploaded the whole song
                    let song_base64 = remove_data_url_prefix_from_base64_string(event.target.result);
                    let total_bytes_length = song_base64.length;
                    let parts = split_string_into_smaller_parts(song_base64, 400);

                    if (can_start_file_upload() == true)
                    {
                        file_send_intent = "musicbot_file";

                        file_send_intent_extra_data = {
                            song_name : files[x].name,
                            musicbot_id: parseInt(selected_client_id)
                        };

                        send_file_by_parts(parts, total_bytes_length, 5);
                    }
                }
                reader.readAsDataURL(file); // invokes onload
            }
        }
    }
};

var webrtc = {
    // sends each newly gathered ice candidate to the server through the signaling websocket
    peerconnection_handle_ice_candidate_event: function(event)
    {
        if (event.candidate)
        {
            console.log(event.candidate);

            console.log("send_ice_candidate_to_server");

            let message_object = {
                message: {
                    type: "ice_candidate",
                    value: event.candidate
                }
            };

            console.log(message_object);

            send_message_object(message_object);
        }
    },

    // logs ice connection state changes for debugging
    peerconnection_handle_ice_connection_state_change_event: function(event)
    {
        console.log("peerconnection_handle_ice_connection_state_change_event " + event.target.iceConnectionState);
    },

    // logs when ice candidate gathering begins and when it is finished
    peerconnection_handle_ice_gathering_state_change_event: function(event)
    {
        switch (event.target.iceGatheringState)
        {
            case "gethering":
                console.log("collection of candinates has begun");
                break;

            case "complete":
                console.log("collection of candinates is finished");
                break;
        }
    },

    // logs signaling state changes for debugging
    peerconnection_handle_signaling_state_change_event: function(event)
    {
        console.log("peerconnection_handle_signaling_state_change_event : " + event.target.signalingState);
    },

    // logs that negotiation is needed, no renegotiation is actually started here
    peerconnection_handle_negotiation_needed_event: function(event)
    {
        console.log("negotiation needed");
    },

    // logs the overall peer connection state whenever it changes
    peer_connection_handle_onconnection_state_change_event: function()
    {
        if (g_peer_connection_with_server != null)
        {
            console.log("peer_connection_with_server connectionState changed to " + g_peer_connection_with_server.connectionState);
        }
    },

    // stores the datachannel opened by the server, wires up its handlers,
    // and activates the microphone right away if always-on mode is enabled
    peerconnection_on_datachannel_receive: function(event)
    {
        // only the current peer connection may install the channel: a replaced pc finishing
        // its handshake late used to hijack g_datachannel and fake the connected flag
        if (event.currentTarget !== g_peer_connection_with_server)
        {
            console.log("datachannel from a replaced peer connection ignored");
            try { event.channel.close(); } catch (stale_channel_close_error) { }
            return;
        }

        g_datachannel = event.channel;
        console.log(`"DataChannel received with label "${event.channel.label}"`);
        event.channel.onopen = webrtc.datachannel_onopen;
        event.channel.onclose = webrtc.datachannel_onclose;
        event.channel.onmessage = webrtc.datachannel_onmessage;
        event.channel.binaryType = "arraybuffer";
        g_is_webrtc_datachannel_connected = true;

        if (g_is_microphone_always_on)
        {
            activate_microphone();
        }
    },

    // shows the push-to-talk / toggle microphone control for this device type
    // and sets the local client's audio state to microphone disabled
    datachannel_onopen: function()
    {
        console.log("datachannelonopen");

        let is_client_existing = get_client_index_in_array_by_client_id(local_client_id) != -1;

        // not found
        if (is_client_existing == false)
        {
            return;
        }

        let target_element = document.getElementById("client-audio-state-" + local_client_id);

        if (g_is_client_running_under_touch_device)
        {
            g_is_microphone_available = true;
            update_microphone_button();
        }
        else
        {
            document.getElementById("toggle-microphone-label").style.display = "block";
        }

        let local_index = get_client_index_in_array_by_client_id(local_client_id);

        if (local_index != -1)
        {
            g_client_list[local_index].audio_state = AUDIO_STATE.PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS;
        }

        if (target_element.classList.contains("client-audio-state-completely-disabled"))
        {
            document.getElementById("client-audio-state-" + local_client_id).classList.remove("client-audio-state-completely-disabled");
        }

        document.getElementById("client-audio-state-" + local_client_id).classList.add("client-audio-state-microphone-disabled");

    },

    // hides the microphone controls when the datachannel closes, drops the connected flag
    // and restarts the connection check so a silent server-side teardown cannot strand us
    datachannel_onclose: function(event)
    {
        console.log("daatachannel_onclose");

        // a replaced or already-torn-down channel closing late must not touch current state
        // (enter_deep_idle and pc replacement both null g_datachannel before closing)
        if (event != null && event.target !== g_datachannel)
        {
            return;
        }

        g_is_microphone_available = false;
        update_microphone_button();
        document.getElementById("toggle-microphone-label").style.display = "none";

        // the server's disabled-audio broadcast was the only recovery trigger here, and that
        // edge can be lost when the server slot is wiped first - recover from our own close too
        g_is_webrtc_datachannel_connected = false;

        if (g_is_voice_chat_allowed_by_server == true && g_is_deep_idle == false
            && g_is_webrtc_datachannel_check_running == false)
        {
            g_is_webrtc_datachannel_check_running = true;
            webrtc_datachannel_connection_check(true);
        }
    },

    // this is the very important handler that receives UDP audio data voice / music! from webrtc datahcannel!
    datachannel_onmessage: function(event)
    {
        if (event.data != null && event.data.byteLength > 0) { g_session_bytes_received += event.data.byteLength; }

        // find out if client is ignored or muted, before sending it to opus g_decoder

        let dataView = new DataView(event.data);
        let extracted_client_id = dataView.getInt32(0, true);     // clientid is always first 4 bytes of received chunk of bytes
        if (extracted_client_id == -2)
        {
            // old servers mark every music bot frame with the constant -2; new servers send the
            // bot's real client id instead (handled below), so each bot gets its own g_decoder
            g_opus_decoder_worker.postMessage({
                type: "mainthread__add_data_to_opus_decoder_musicbot",
                value: event.data
            });
        }
        else
        {
            let client_index = get_client_index_in_array_by_client_id(extracted_client_id);

            if (client_index == -1 || extracted_client_id == local_client_id)
            {
                return; // not found
            }

            if (g_client_list[client_index].is_muted_by_local_client == true || g_client_list[client_index].is_ignored_by_local_client == true)
            {
                return;
            }

            // audio from a channel we are not in is a stale straggler (e.g. after a switch): discard
            if (g_client_list[client_index].channel_id != current_channel_id)
            {
                return;
            }

            // music bot frames are plaintext opus (no channel-key layer), voice frames are encrypted;
            // the sender's is_music_bot flag from the client list picks the right worker path
            if (g_client_list[client_index].is_music_bot == true)
            {
                g_opus_decoder_worker.postMessage({
                    type: "mainthread__add_data_to_opus_decoder_musicbot",
                    value: event.data
                });
            }
            else
            {
                g_opus_decoder_worker.postMessage({
                    type: "mainthread__add_data_to_opus_decoder",
                    value: event.data
                });
            }
        }
    }
}
