// builds conversation previews from messages already retained in this client
// reuses existing member lists and chat contexts without extra network requests
var member_conversations =
{
    active: false,
    auto_open: false,
    is_room_list_shown: false,
    is_tab_bar_shown: false,
    observer: null,
    pending: false,
    participant_events_bound: false,
    participant_focus: null
};

/**
 * @brief applies the theme's conversation layout and starts or stops its list observer
 *
 * @return void
 */
function member_conversations__apply_theme()
{
    // CSS enables these features without coupling the module to particular themes
    let style = getComputedStyle(document.documentElement);
    member_conversations.active = style.getPropertyValue("--member-conversations").trim() === "1";
    member_conversations.auto_open = style.getPropertyValue("--member-conversations-auto-open").trim() === "1";
    member_conversations.is_room_list_shown = member_conversations.active && style.getPropertyValue("--member-conversations-rooms").trim() === "1";
    member_conversations.is_tab_bar_shown = member_conversations.active && style.getPropertyValue("--member-conversations-tabs").trim() === "1";
    document.body.classList.toggle("member-conversations-active", member_conversations.active);
    member_conversations__apply_tab_bar();
    member_conversations__place_connection_menu();
    member_conversations__bind_participant_events();
    member_conversations__update_participant_triggers();
    member_conversations__refresh_participant_dialog();
    if (member_conversations.observer != null)
    {
        member_conversations.observer.disconnect();
    }
    if (!member_conversations.active)
    {
        for (let participants of document.querySelectorAll(".member-conversation-participants"))
        {
            participants.remove();
        }
        member_conversations__refresh_rooms();
        return;
    }

    let list = document.getElementById("member-list-container");
    if (document.getElementById("member-conversations-search") == null)
    {
        let search = document.createElement("label");
        search.id = "member-conversations-search";
        let input = document.createElement("input");
        input.type = "search";
        input.id = "member-conversations-search-input";
        input.placeholder = "Search people and channels";
        input.setAttribute("aria-label", "Search people and channels");
        input.autocomplete = "off";
        input.addEventListener("input", member_conversations__filter_people);
        search.appendChild(input);
        list.prepend(search);

        let empty = document.createElement("p");
        empty.id = "member-conversations-search-empty";
        empty.setAttribute("role", "status");
        empty.hidden = true;
        empty.textContent = "No matching people or channels";
        list.appendChild(empty);

        let profile = document.createElement("div");
        profile.id = "member-conversations-profile";
        profile.setAttribute("aria-hidden", "true");
        document.getElementById("communication-system-head-menu").appendChild(profile);
    }
    if (member_conversations.observer == null)
    {
        member_conversations.observer = new MutationObserver(member_conversations__schedule_refresh);
    }
    member_conversations.observer.observe(document.getElementById("chat-context-container"),
    {
        childList: true,
        subtree: true,
        characterData: true
    });
    // a row added or removed changes the names shown in the row of the channel
    member_conversations.observer.observe(document.getElementById("member-list-body"),
    {
        childList: true
    });
    // initialize the picture and preview when a conversation tab is created
    member_conversations.observer.observe(document.getElementById("chat-context-selectors-container"),
    {
        childList: true
    });
    member_conversations__schedule_refresh();
}

// place existing connection actions in Settings for conversation list themes
// restore their toolbar position when leaving those themes
/**
 * @brief moves connection controls into settings for room lists and restores their toolbar position otherwise
 *
 * @return void
 */
function member_conversations__place_connection_menu()
{
    let menu = document.getElementById("connections-menu");
    let settings = document.getElementById("member-conversations-connection-settings");
    if (member_conversations.is_room_list_shown)
    {
        if (settings == null)
        {
            settings = document.createElement("div");
            settings.id = "member-conversations-connection-settings";
            let heading = document.createElement("h2");
            heading.className = "local-settings-section-title";
            heading.textContent = "Connection";
            settings.appendChild(heading);
            document.getElementById("local-settings-page-advanced").appendChild(settings);
        }
        if (menu.parentElement != settings)
        {
            member_conversations.connection_menu_home = document.createComment("Connection menu toolbar position");
            menu.before(member_conversations.connection_menu_home);
            settings.appendChild(menu);
            head_menu__close_all();
        }
        settings.hidden = false;
    }
    else if (settings != null)
    {
        if (member_conversations.connection_menu_home != null)
        {
            member_conversations.connection_menu_home.replaceWith(menu);
            member_conversations.connection_menu_home = null;
            head_menu__close_all();
        }
        settings.hidden = true;
    }
}

/**
 * @brief queues one conversation list refresh for the next animation frame
 *
 * @return void
 */
function member_conversations__schedule_refresh()
{
    if (!member_conversations.active || member_conversations.pending)
    {
        return;
    }
    member_conversations.pending = true;
    requestAnimationFrame(function()
    {
        member_conversations.pending = false;
        member_conversations__refresh_people();
    });
}

/**
 * @brief refreshes profile, message previews, channel participants and conversation list filtering
 *
 * @return void
 */
function member_conversations__refresh_people()
{
    if (!member_conversations.active)
    {
        return;
    }
    let body = document.getElementById("member-list-body");
    let profile = document.getElementById("member-conversations-profile");
    let local = body.querySelector(".connected-local-client");
    let current_channel = g_channel_list.find(function(channel)
    {
        return Number(channel.channel_id) === Number(g_current_channel_id);
    });

    // the screen chosen in the bar decides whether the chat is shown
    if (member_conversations.is_tab_bar_shown && ((document.body.dataset.conversationTab == "chat") == g_is_chat_hidden) && document.body.dataset.conversationTab != "people")
    {
        UI.hide_chat_container();
    }

    // mark the user's channel in the channel tree
    for (let row of document.querySelectorAll("#channel-list-container .single-channel"))
    {
        row.classList.toggle("single-channel-current", Number(row.dataset.channelId) === Number(g_current_channel_id));
    }

    // a channel without audio gets no talk bar
    document.body.classList.toggle("current-channel-audio-disabled", current_channel != null && current_channel.is_audio_enabled == false);
    if (profile != null)
    {
        profile.replaceChildren();
    }
    if (profile != null && local != null)
    {
        let avatar = local.querySelector(".client-audio-state");
        if (avatar != null)
        {
            profile.appendChild(avatar.cloneNode(true));
        }
        let name = document.createElement("strong");
        name.textContent = g_local_username;
        profile.appendChild(name);
        let caption = document.createElement("span");
        let channel = local.querySelector(".member-list-client-channel");
        caption.textContent = channel != null ? "In " + channel.textContent : "Connected";
        profile.appendChild(caption);
    }

    // update channel list
    member_conversations__refresh_rooms();

    // mark new messages
    member_conversations__mark_messages();

    // conversation lists use the channel icon as a picker. tabbed layouts keep the header shortcut
    document.getElementById("member-list-header").innerHTML = member_conversations.is_room_list_shown ? "Chats" : "People &amp; chats<button type='button' id='pocket-channel-switch' aria-haspopup='menu'>Switch channel</button>";
    for (let row of body.querySelectorAll(".member-list-client, .member-list-channel"))
    {
        let context_id = "";
        if (row.classList.contains("member-list-channel"))
        {
            context_id = "chat-context-channel-" + row.getAttribute("data-member-list-channel-id");
        }
        else if (row.classList.contains("offline-contact"))
        {
            context_id = "chat-context-offline-" + row.dataset.identityHash;
        }
        else
        {
            context_id = "chat-context-pm-" + row.getAttribute("data-connected-client-id");
        }
        let context = document.getElementById(context_id);
        let last = context != null ? context.lastElementChild : null;
        while (last != null && !last.classList.contains("single-chat-message"))
        {
            last = last.previousElementSibling;
        }
        let preview = row.querySelector(".member-conversation-preview");
        let timestamp = row.querySelector(".member-conversation-time");
        if (preview == null)
        {
            preview = document.createElement("span");
            preview.className = "member-conversation-preview";
            row.appendChild(preview);
            timestamp = document.createElement("span");
            timestamp.className = "member-conversation-time";
            row.appendChild(timestamp);
        }
        let text = "";
        let time = "";
        if (last != null)
        {
            let paragraph = last.querySelector(".single-chat-message-content-p");
            let content = last.querySelector(".single-chat-message-content");
            text = paragraph != null ? paragraph.textContent : content != null ? content.textContent : "";
            text = text.replace(/\s+/g, " ").trim();
            if (!text && content != null && content.querySelector("img") != null)
            {
                text = "Picture";
            }
            if (text && last.querySelector(".single-chat-message-sender-local-username-container") != null)
            {
                text = "You: " + text;
            }
            let sent = last.querySelector(".single-chat-message-sender-time > p");
            time = sent != null ? sent.textContent.trim().replace(/(\d{1,2}:\d{2}):\d{2}/, "$1") : "";
        }

        // line about the conversation
        let status = "";
        if (row.classList.contains("member-list-channel"))
        {
            let sender = last != null ? last.querySelector(".single-chat-message-sender-username-container") : null;
            status = member_conversations__channel_people_text(row.getAttribute("data-member-list-channel-id"));
            text = text && sender != null ? sender.textContent.trim() + ": " + text : text;
        }
        else
        {
            let channel = row.querySelector(".member-list-client-channel");
            let presence = row.querySelector(".member-list-presence");
            status = !row.classList.contains("in-current-channel") && channel != null && channel.textContent.trim() != "" ? "In " + channel.textContent : presence != null ? presence.textContent : "";
        }

        // mark the title
        member_conversations__mark_title(row, context_id, status);

        // keep user text as text, including when it resembles markup
        let empty_preview = row.classList.contains("member-list-channel") ? status : "No messages yet";
        if (row.classList.contains("music-bot-client"))
        {
            empty_preview = "Music bot · " + status;
            let avatar = row.querySelector(".client-audio-state");
            if (avatar != null && !avatar.style.backgroundImage && !avatar.style.getPropertyValue("--client-avatar"))
            {
                avatar.dataset.initials = "♫";
            }
        }
        preview.textContent = (text || empty_preview).slice(0, 200);
        preview.classList.toggle("member-conversation-preview-empty", !text);
        timestamp.textContent = time;

        if (row.classList.contains("member-list-channel"))
        {
            member_conversations__render_participants(row, row.getAttribute("data-member-list-channel-id"), preview);
        }
    }
    // update every open channel tab, including an older channel whose chat is still being read
    for (let tab of document.querySelectorAll('.chat-context-selector[data-chat-context-selector-type="channel"]'))
    {
        let channel_id = tab.dataset.chatContextSelectorId.replace("channel-", "");
        tab.dataset.conversationStatus = member_conversations__channel_people_text(channel_id);
        member_conversations__render_participants(tab, channel_id);
    }
    member_conversations__update_participant_triggers();
    member_conversations__refresh_participant_dialog();
    member_conversations__filter_people();
}

/**
 * @brief creates and toggles the four screen buttons when --member-conversations-tabs is enabled
 *
 * @return void
 */
function member_conversations__apply_tab_bar()
{
    let bar = document.getElementById("member-conversations-tabs");
    let screens = [["people", "People"], ["chat", "Chat"], ["channels", "Channels"], ["me", "Me"]];

    // nothing to hide
    if (bar == null && member_conversations.is_tab_bar_shown == false)
    {
        return;
    }

    // create the bar once
    if (bar == null)
    {
        bar = document.createElement("nav");
        bar.id = "member-conversations-tabs";
        bar.setAttribute("aria-label", "Screens");
        for (let [name, label] of screens)
        {
            let button = document.createElement("button");
            button.type = "button";
            button.dataset.tab = name;
            button.textContent = label;
            bar.appendChild(button);
        }
        bar.addEventListener("click", member_conversations__tab_onclick);
        document.getElementById("body-container").appendChild(bar);
        document.getElementById("channel-list-container").addEventListener("click", member_conversations__tree_onclick, true);
    }

    // show or hide
    bar.hidden = !member_conversations.is_tab_bar_shown;
    if (member_conversations.is_tab_bar_shown == false)
    {
        delete document.body.dataset.conversationTab;
        member_conversations__set_action_labels(false);
        return;
    }

    // open Chat initially, regardless of its position in the bar
    if (document.body.dataset.conversationTab == null)
    {
        member_conversations__mark_tab("chat");
    }
}

/**
 * @brief stores which screen of the bar is shown and marks its button as pressed
 *
 * @param string name -> "chat", "people", "channels" or "me"
 *
 * @return void
 */
function member_conversations__mark_tab(name)
{
    // remember the screen
    document.body.dataset.conversationTab = name;
    member_conversations__set_action_labels(name === "me");

    // mark its button
    for (let button of document.querySelectorAll("#member-conversations-tabs > button"))
    {
        button.setAttribute("aria-pressed", button.dataset.tab == name ? "true" : "false");
    }
}

/**
 * @brief uses full labels on the profile actions and restores compact toolbar labels elsewhere
 *
 * @param boolean profile_is_shown -> whether the profile screen is open
 *
 * @return void
 */
function member_conversations__set_action_labels(profile_is_shown)
{
    for (let [id, label] of [["choose-theme-button", "Appearance"], ["local-settings-button", "Settings"]])
    {
        let button = document.getElementById(id);
        if (button != null)
        {
            if (button.dataset.toolbarLabel == null)
            {
                button.dataset.toolbarLabel = button.value;
            }
            button.value = profile_is_shown ? label : button.dataset.toolbarLabel;
        }
    }
}

/**
 * @brief shows the selected screen, opening the user's channel when Chat is chosen
 *
 * @param string name -> "chat", "people", "channels" or "me"
 *
 * @return void
 */
function member_conversations__show_tab(name)
{
    member_conversations__mark_tab(name);

    // remove the join question
    member_conversations__ask_join(null);

    // open the channel chat
    if (name == "chat")
    {
        member_conversations__open_channel_chat(g_current_channel_id);
    }

    // show or hide the chat
    if ((name != "chat") != g_is_chat_hidden)
    {
        UI.hide_chat_container();
    }
}

/**
 * @brief shows the screen selected in the navigation bar
 *
 * @param object event -> the click event
 *
 * @return void
 */
function member_conversations__tab_onclick(event)
{
    let button = event.target.closest("button[data-tab]");

    // not a button
    if (button == null)
    {
        return;
    }
    member_conversations__show_tab(button.dataset.tab);
}

/**
 * @brief selects a conversation or shows a channel's Join control
 *
 * @param object event -> the click event
 *
 * @return void
 */
function member_conversations__tree_onclick(event)
{
    let channel = event.target.closest(".single-channel");
    let client = event.target.closest(".connected-client");
    let join_button = event.target.closest(".single-channel-join");

    // only with the bar
    if (member_conversations.is_tab_bar_shown == false)
    {
        return;
    }

    // join after the question
    if (join_button != null)
    {
        event.stopPropagation();
        member_conversations__join_channel(channel.dataset.channelId);
        member_conversations__ask_join(null);
        return;
    }

    // ask before joining
    if (channel != null)
    {
        if (event.target.closest(".single-channel-collapse-button") == null)
        {
            member_conversations__ask_join(channel);
        }
        return;
    }

    // show the conversation
    if (client != null && !client.classList.contains("connected-local-client"))
    {
        setTimeout(function()
        {
            // not selected
            if (g_current_chat_context_id !== "chat-context-pm-" + client.getAttribute("data-connected-client-id"))
            {
                return;
            }
            member_conversations__mark_tab("people");
            if (g_is_chat_hidden == true)
            {
                UI.hide_chat_container();
            }
        }, 0);
    }
}

/**
 * @brief toggles the Join button for a channel other than the current channel
 *
 * @param object channel -> the channel row (.single-channel), or null to remove the button everywhere
 *
 * @return void
 */
function member_conversations__ask_join(channel)
{
    let is_shown = channel != null && channel.querySelector(":scope > .single-channel-join") != null;
    let button = null;

    // remove the old button
    for (let old of document.querySelectorAll("#channel-list-container .single-channel-join"))
    {
        old.remove();
    }

    // nothing to ask
    if (channel == null || is_shown || Number(channel.dataset.channelId) === Number(g_current_channel_id))
    {
        return;
    }

    // show the button
    button = document.createElement("button");
    button.type = "button";
    button.className = "single-channel-join";
    button.textContent = "Join";
    channel.appendChild(button);
}

/**
 * @brief collects people and music bots in a channel, with the local user first
 *
 * @param number|string channel_id -> channel to inspect
 *
 * @return array -> participants excluding idle clients
 */
function member_conversations__channel_participants(channel_id)
{
    let participants = [];
    for (let client of g_client_list)
    {
        if (Number(client.channel_id) !== Number(channel_id) || client.is_idle == true)
        {
            continue;
        }
        if (Number(client.client_id) === Number(g_local_client_id))
        {
            participants.unshift(client);
        }
        else
        {
            participants.push(client);
        }
    }
    return participants;
}

/**
 * @brief returns the names of people and music bots in a channel as one line of text
 *
 * @param string channel_id -> id of the channel
 * @param array|undefined participants -> optional participant list, otherwise collected from the channel
 *
 * @return string -> for example "Only you are here", "You, anna, peter" or "You, anna, peter, tom and 2 more"
 */
function member_conversations__channel_people_text(channel_id, participants)
{
    let names = [];
    let is_local_inside = false;
    let shown = "";

    // collect the names
    for (let client of participants || member_conversations__channel_participants(channel_id))
    {
        if (Number(client.client_id) === Number(g_local_client_id))
        {
            is_local_inside = true;
        }
        else
        {
            names.push(client.username);
        }
    }

    // nobody else
    if (names.length == 0)
    {
        return is_local_inside ? "Only you are here" : "Nobody is here";
    }

    // shorten a long list
    shown = names.slice(0, 3).join(", ");
    if (names.length > 3)
    {
        shown += " and " + (names.length - 3) + " more";
    }
    return (is_local_inside ? "You, " : "") + shown;
}

/**
 * @brief shows a bounded row of participant avatars beside the channel names
 *
 * @param Element container -> channel header or conversation row
 * @param number|string channel_id -> channel whose participants are shown
 * @param Element|null before -> insert before this child, or append when absent
 *
 * @return void
 */
function member_conversations__render_participants(container, channel_id, before)
{
    let summary = container.querySelector(":scope > .member-conversation-participants");
    if (!member_conversations.is_room_list_shown && !member_conversations.is_tab_bar_shown)
    {
        if (summary != null)
        {
            summary.remove();
        }
        return;
    }

    if (summary == null)
    {
        summary = document.createElement("span");
        summary.className = "member-conversation-participants";
        container.insertBefore(summary, before || null);
    }
    let participants = member_conversations__channel_participants(channel_id);
    let avatars = document.createElement("span");
    avatars.className = "member-conversation-avatars";
    avatars.setAttribute("aria-hidden", "true");
    let full_names = [];
    for (let client of participants)
    {
        let name = String(client.username || "");
        let label = name + (Number(client.client_id) === Number(g_local_client_id) ? " (you)" : client.is_music_bot == true ? " (music bot)" : "");
        full_names.push(label);
        if (avatars.children.length >= 3)
        {
            continue;
        }
        avatars.appendChild(member_conversations__participant_avatar(client));
    }
    if (participants.length > 3)
    {
        let more = document.createElement("span");
        more.className = "member-conversation-avatar-more";
        more.textContent = "+" + (participants.length - 3);
        avatars.appendChild(more);
    }
    let names = document.createElement("span");
    names.className = "member-conversation-participant-names";
    names.textContent = member_conversations__channel_people_text(channel_id, participants);
    summary.title = full_names.join(", ") || "Nobody is here";
    summary.replaceChildren(avatars, names);
}

/**
 * @brief builds a participant avatar with an allowed image, initials or a music bot symbol
 *
 * @param object client -> channel participant record
 *
 * @return HTMLSpanElement -> decorative participant avatar
 */
function member_conversations__participant_avatar(client)
{
    let name = String(client.username || "");
    let avatar = document.createElement("span");
    avatar.className = "member-conversation-avatar";
    avatar.dataset.clientId = client.client_id;
    avatar.title = name + (Number(client.client_id) === Number(g_local_client_id) ? " (you)" : client.is_music_bot == true ? " (music bot)" : "");
    avatar.setAttribute("aria-hidden", "true");
    let image = g_server_policy.is_avatar_upload_allowed == true && typeof client.base64_avatar === "string" ? client.base64_avatar : "";
    if (image)
    {
        avatar.style.backgroundImage = "url(" + JSON.stringify(image) + ")";
    }
    else
    {
        avatar.textContent = client.is_music_bot == true ? "♫" : UI.initials_for_name(name);
    }
    return avatar;
}

/**
 * @brief makes channel headers accessible participant buttons only in supported layouts
 *
 * @return void
 */
function member_conversations__update_participant_triggers()
{
    for (let tab of document.querySelectorAll('.chat-context-selector[data-chat-context-selector-type="channel"]'))
    {
        if (member_conversations.is_room_list_shown || member_conversations.is_tab_bar_shown)
        {
            tab.classList.add("channel-participants-trigger");
            tab.tabIndex = 0;
            tab.setAttribute("role", "button");
            tab.setAttribute("aria-haspopup", "dialog");
            let title = tab.querySelector(".p-container, p");
            tab.setAttribute("aria-label", "Show participants in " + (title != null ? title.textContent.trim() : "this channel"));
            tab.title = "Show everyone in this channel";
        }
        else if (tab.classList.contains("channel-participants-trigger"))
        {
            tab.classList.remove("channel-participants-trigger");
            for (let attribute of ["tabindex", "role", "aria-haspopup", "aria-label", "title"])
            {
                tab.removeAttribute(attribute);
            }
        }
    }
}

/**
 * @brief binds participant dialog gestures once on the channel header container
 *
 * @return void
 */
function member_conversations__bind_participant_events()
{
    let tabs = document.getElementById("chat-context-selectors-container");
    if (member_conversations.participant_events_bound || tabs == null)
    {
        return;
    }
    member_conversations.participant_events_bound = true;
    /**
     * @brief opens participants from a trusted click or activation key on the active channel header
     *
     * @param MouseEvent|KeyboardEvent event -> channel header interaction
     *
     * @return void
     */
    function open_from_header(event)
    {
        // programmatic tab.click() still opens the conversation from the chat list
        if (!event.isTrusted || (!member_conversations.is_room_list_shown && !member_conversations.is_tab_bar_shown) || g_is_chat_hidden)
        {
            return;
        }
        if (event.type === "keydown" && event.key !== "Enter" && event.key !== " ")
        {
            return;
        }
        let tab = event.target.closest('.channel-participants-trigger[style*="background-color"]');
        if (tab == null)
        {
            return;
        }
        event.preventDefault();
        event.stopPropagation();
        member_conversations__open_participants(tab.dataset.chatContextSelectorId.replace("channel-", ""), tab);
    }
    tabs.addEventListener("click", open_from_header, true);
    tabs.addEventListener("keydown", open_from_header, true);
}

/**
 * @brief opens the participant list without changing the current channel or conversation
 *
 * @param number|string channel_id -> channel to list
 * @param Element trigger -> control to focus when the dialog closes
 *
 * @return void
 */
function member_conversations__open_participants(channel_id, trigger)
{
    let dialog = document.getElementById("channel-participants-dialog");
    if (dialog == null)
    {
        dialog = document.createElement("dialog");
        dialog.id = "channel-participants-dialog";
        dialog.setAttribute("aria-labelledby", "channel-participants-title");
        dialog.setAttribute("aria-describedby", "channel-participants-count");
        dialog.innerHTML = '<div class="channel-participants-heading"><h2 id="channel-participants-title"></h2>' + '<button type="button" id="channel-participants-close" aria-label="Close participants">×</button></div>' + '<p id="channel-participants-count"></p>' + '<input type="search" id="channel-participants-search" placeholder="Search participants" aria-label="Search participants" autocomplete="off">' + '<ul id="channel-participants-list" tabindex="0" aria-label="Channel participants"></ul>' + '<p id="channel-participants-empty" role="status" hidden></p>';
        document.body.appendChild(dialog);
        document.getElementById("channel-participants-close").addEventListener("click", function()
        {
            dialog.close();
        });
        document.getElementById("channel-participants-search").addEventListener("input", member_conversations__refresh_participant_dialog);
        dialog.addEventListener("click", function(event)
        {
            let bounds = dialog.getBoundingClientRect();
            if (event.target === dialog && (event.clientX < bounds.left || event.clientX > bounds.right || event.clientY < bounds.top || event.clientY > bounds.bottom))
            {
                dialog.close();
            }
        });
        dialog.addEventListener("close", function()
        {
            let previous = member_conversations.participant_focus;
            member_conversations.participant_focus = null;
            if (previous != null && previous.isConnected && previous.getClientRects().length > 0)
            {
                previous.focus({ preventScroll: true });
            }
        });
    }
    UI.delete_contextmenus(true);
    member_conversations.participant_focus = trigger;
    dialog.dataset.channelId = channel_id;
    document.getElementById("channel-participants-search").value = "";
    if (!dialog.open)
    {
        dialog.showModal();
    }
    member_conversations__refresh_participant_dialog();
    // focus Close initially so opening the list does not raise the phone keyboard
    document.getElementById("channel-participants-close").focus({ preventScroll: true });
}

/**
 * @brief refreshes an open participant dialog while preserving its scroll position and search
 *
 * @return void
 */
function member_conversations__refresh_participant_dialog()
{
    let dialog = document.getElementById("channel-participants-dialog");
    if (dialog == null || !dialog.open)
    {
        return;
    }
    let channel = g_channel_list.find(function(entry)
    {
        return Number(entry.channel_id) === Number(dialog.dataset.channelId);
    });
    if (channel == null || (!member_conversations.is_room_list_shown && !member_conversations.is_tab_bar_shown))
    {
        dialog.close();
        return;
    }
    let participants = member_conversations__channel_participants(channel.channel_id);
    document.getElementById("channel-participants-title").textContent = channel.name;
    document.getElementById("channel-participants-count").textContent = participants.length + (participants.length === 1 ? " participant" : " participants");
    let query = document.getElementById("channel-participants-search").value.trim().toLocaleLowerCase();
    let list = document.getElementById("channel-participants-list");
    let scroll = list.scrollTop;
    let rows = document.createDocumentFragment();
    let matches = 0;
    for (let client of participants)
    {
        let row = document.createElement("li");
        row.className = "channel-participant-row";
        row.dataset.clientId = client.client_id;
        let name = document.createElement("span");
        name.className = "channel-participant-name";
        name.textContent = String(client.username || "");
        let description = document.createElement("span");
        description.className = "channel-participant-description";
        description.textContent = Number(client.client_id) === Number(g_local_client_id) ? "You" : client.is_music_bot == true ? "Music bot" : "In this channel";
        row.hidden = !(name.textContent + " " + description.textContent).toLocaleLowerCase().includes(query);
        if (!row.hidden)
        {
            matches++;
        }
        row.append(member_conversations__participant_avatar(client), name, description);
        rows.appendChild(row);
    }
    list.replaceChildren(rows);
    list.scrollTop = scroll;
    let empty = document.getElementById("channel-participants-empty");
    empty.textContent = participants.length === 0 ? "Nobody is here" : "No matching participants";
    empty.hidden = matches !== 0;
}

/**
 * @brief updates the conversation tab's status, initials and avatar for the phone header
 *
 * @param object row -> the row of the conversation in the list
 * @param string context_id -> id of the conversation's chat context, for example chat-context-pm-3
 * @param string status -> the line of text
 *
 * @return void
 */
function member_conversations__mark_title(row, context_id, status)
{
    let tab = document.querySelector('.chat-context-selector[data-chat-context-selector-id="' + context_id.replace("chat-context-", "").replace("pm-", "user-") + '"]');
    let picture = row.querySelector(".client-audio-state, .member-list-channel-icon");
    let image = "";

    // conversation not opened
    if (tab == null)
    {
        return;
    }

    // uploaded picture
    if (picture != null)
    {
        image = (picture.style.getPropertyValue("--client-avatar") || picture.style.backgroundImage).trim();
    }

    // write the marks
    tab.dataset.conversationStatus = status;
    tab.dataset.conversationInitials = image ? "" : row.classList.contains("member-list-channel") ? "#" : picture != null && picture.dataset.initials ? picture.dataset.initials : "";
    if (tab.style.getPropertyValue("--conversation-picture") !== (image || "none"))
    {
        tab.style.setProperty("--conversation-picture", image || "none");
    }
}

/**
 * @brief makes channel icons accessible channel pickers
 * the remaining row opens the conversation. holding the row also opens the picker
 *
 * @return void
 */
function member_conversations__refresh_rooms()
{
    for (let icon of document.querySelectorAll("#member-list-body .member-list-channel-icon"))
    {
        let row = icon.closest(".member-list-channel");
        let name = row.querySelector(".member-list-channel-name");
        icon.classList.toggle("member-conversations-channel-switch", member_conversations.is_room_list_shown);
        if (member_conversations.is_room_list_shown)
        {
            row.setAttribute("role", "group");
            row.removeAttribute("tabindex");
            name.setAttribute("role", "button");
            name.setAttribute("aria-label", "Open " + name.textContent + " chat");
            name.tabIndex = 0;
            icon.setAttribute("role", "button");
            icon.tabIndex = 0;
            icon.setAttribute("aria-label", "Switch channel");
            icon.setAttribute("aria-haspopup", "menu");
            icon.title = "Switch channel (tap or hold)";
        }
        else
        {
            row.setAttribute("role", "button");
            row.tabIndex = 0;
            for (let attribute of ["role", "tabindex", "aria-label"])
            {
                name.removeAttribute(attribute);
            }
            for (let attribute of ["role", "tabindex", "aria-label", "aria-haspopup", "title"])
            {
                icon.removeAttribute(attribute);
            }
        }
    }
}

/**
 * @brief adds short timestamps and repeated sender marks to phone message bubbles
 *
 * @return void
 */
function member_conversations__mark_messages()
{
    for (let message of document.querySelectorAll("#chat-context-container .single-chat-message:not([data-sender-key])"))
    {
        let time = message.querySelector(".single-chat-message-sender-time > p");
        let sender = message.querySelector(".single-chat-message-sender-username-container, .single-chat-message-sender-local-username-container");
        let previous = message.previousElementSibling;

        // time without seconds
        if (time != null)
        {
            time.dataset.shortTime = time.textContent.trim().replace(/(\d{1,2}:\d{2}):\d{2}/, "$1");
        }

        // same sender as before
        message.dataset.senderKey = sender != null ? sender.className + " " + sender.textContent.trim() : "";
        message.classList.toggle("chat-message-same-sender", previous != null && previous.dataset.senderKey === message.dataset.senderKey);
    }
}

/**
 * @brief shows the chat of a channel, the same way as a press on its conversation tab
 *        A tab that was just written into the page has no handler yet and gets the standard one
 *
 * @param number channel_id -> id of the channel whose chat is shown
 *
 * @return void
 */
function member_conversations__open_channel_chat(channel_id)
{
    let channel_tab = document.querySelector('.chat-context-selector[data-chat-context-selector-id="channel-' + channel_id + '"]');

    // no such tab
    if (channel_tab == null)
    {
        return;
    }

    // add missing handler
    if (channel_tab.onclick == null)
    {
        channel_tab.onclick = UI.chat_context_selector_onclick;
    }

    // show the chat
    channel_tab.click();
    member_conversations__show_selected_chat();
}

/**
 * @brief joins a channel through the existing tree handler, including its password prompt
 *
 * @param number channel_id -> id of the channel to join
 *
 * @return void
 */
function member_conversations__join_channel(channel_id)
{
    let name_element = document.querySelector('#channel-list-container .single-channel[data-channel-id="' + channel_id + '"] [data-channel-name-id]');
    if (name_element != null)
    {
        UI.single_channel_doubleclick_join({ stopPropagation: function()
        {
        }, target: name_element });
    }
}

/**
 * @brief filters conversation rows by name or channel and updates the empty result message
 *
 * @return void
 */
function member_conversations__filter_people()
{
    if (!member_conversations.active)
    {
        return;
    }
    let search = document.getElementById("member-conversations-search-input");
    let query = search.value.trim().toLocaleLowerCase();
    let matches = 0;
    for (let row of document.querySelectorAll("#member-list-body > .member-list-client, #member-list-body > .member-list-channel"))
    {
        let name = row.querySelector(".connected-client-p, .connected-client-p-music-bot, .member-list-channel-name");
        let channel = row.querySelector(".member-list-client-channel");
        let searchable = (name != null ? name.textContent : "") + " " + (channel != null ? channel.textContent : "");
        let visible = searchable.toLocaleLowerCase().includes(query);
        row.classList.toggle("member-conversation-filtered", !visible);
        if (visible && !row.classList.contains("connected-local-client"))
        {
            matches++;
        }
    }
    document.getElementById("member-conversations-search-empty").hidden = matches !== 0 || query === "";
}

/**
 * @brief shows and focuses the selected conversation without opening the mobile keyboard
 *
 * @return void
 */
function member_conversations__show_selected_chat()
{
    // only a theme with the conversation list switches from the list to the chat
    if (!member_conversations.active || !member_conversations.auto_open)
    {
        return;
    }
    if (g_is_chat_hidden)
    {
        UI.hide_chat_container();
    }
    document.getElementById("member-conversations-search-input").blur();
    // focus the conversation without raising the phone keyboard until the user types
    let conversation = document.getElementById(g_current_chat_context_id);
    if (conversation != null)
    {
        conversation.tabIndex = -1;
        conversation.focus({ preventScroll: true });
    }
}
