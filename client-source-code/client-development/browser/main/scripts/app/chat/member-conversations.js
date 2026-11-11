// Conversation-list features reuse the member mirror and chat contexts. No separate message store or
// network requests: previews describe only messages already retained in this client.
var member_conversations =
{
    active: false,
    auto_open: false,
    is_room_list_shown: false,
    observer: null,
    pending: false
};

function member_conversations__apply_theme()
{
    // Themes opt in through CSS; no theme names or palettes are coupled to this module.
    let style = getComputedStyle(document.documentElement);
    member_conversations.active = style.getPropertyValue("--member-conversations").trim() === "1";
    member_conversations.auto_open = style.getPropertyValue("--member-conversations-auto-open").trim() === "1";
    member_conversations.is_room_list_shown = style.getPropertyValue("--member-conversations-rooms").trim() === "1";
    document.body.classList.toggle("member-conversations-active", member_conversations.active);
    if (member_conversations.observer != null)
    {
        member_conversations.observer.disconnect();
    }
    if (!member_conversations.active)
    {
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

        // the list of all channels; member_conversations__refresh_rooms fills it when the theme sets --member-conversations-rooms
        let rooms = document.createElement("div");
        rooms.id = "member-conversations-rooms";
        rooms.addEventListener("click", member_conversations__room_onclick);
        search.after(rooms);

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
    member_conversations__schedule_refresh();
}

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

function member_conversations__refresh_people()
{
    if (!member_conversations.active)
    {
        return;
    }
    let body = document.getElementById("member-list-body");
    let profile = document.getElementById("member-conversations-profile");
    let local = body.querySelector(".connected-local-client");
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

    // the button opens the channel list; it has the id of Pocket's button because one click handler in main.js serves both
    document.getElementById("member-list-header").innerHTML = "People &amp; chats<button type='button' id='pocket-channel-switch' aria-haspopup='menu'>Switch channel</button>";
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
        if (!text)
        {
            let channel = row.querySelector(".member-list-client-channel");
            let presence = row.querySelector(".member-list-presence");
            text = channel != null ? channel.textContent : presence != null ? presence.textContent : "Channel chat";
        }
        // Keep user text as text, including when it resembles markup.
        preview.textContent = text.slice(0, 200);
        timestamp.textContent = time;
    }
    member_conversations__filter_people();
}

/**
 * @brief Lists every channel with the names of the people in it, for a theme that sets
 *        --member-conversations-rooms to 1. The list is built again only when its content changed.
 *        A person whose channel this client may not see has channel_id -1 and is in no row.
 *
 * @return void
 */
function member_conversations__refresh_rooms()
{
    let holder = document.getElementById("member-conversations-rooms");
    let rows = [];
    let content = "";

    // list not created
    if (holder == null)
    {
        return;
    }

    // only when wanted
    holder.hidden = !member_conversations.is_room_list_shown;
    if (!member_conversations.is_room_list_shown)
    {
        return;
    }

    // collect the rows
    for (let channel of g_channel_list)
    {
        let names = [];
        for (let client of g_client_list)
        {
            if (Number(client.channel_id) === Number(channel.channel_id) && client.is_idle != true)
            {
                names.push(Number(client.client_id) === Number(g_local_client_id) ? "You" : client.username);
            }
        }
        rows.push([channel.channel_id, channel.name, names.join(", "),
            Number(channel.channel_id) === Number(g_current_channel_id), channel.is_using_password == true]);
    }

    // skip when unchanged
    content = JSON.stringify(rows);
    if (holder.dataset.content === content)
    {
        return;
    }

    // build the rows
    holder.dataset.content = content;
    holder.replaceChildren();
    for (let [channel_id, channel_name, people, is_current, is_locked] of rows)
    {
        let row = document.createElement("button");
        let name = document.createElement("strong");
        let inside = document.createElement("span");
        row.type = "button";
        row.className = "member-conversation-room" + (is_current ? " member-conversation-room-current" : "")
            + (is_locked ? " member-conversation-room-locked" : "");
        row.dataset.channelId = channel_id;
        // user text stays text, also when it looks like markup
        name.textContent = channel_name;
        inside.textContent = people;
        row.append(name, inside);
        holder.appendChild(row);
    }
}

/**
 * @brief A press on a row of the channel list: the row of the channel the user is in opens the chat of
 *        that channel, every other row joins its channel.
 *
 * @param object event -> the click event
 *
 * @return void
 */
function member_conversations__room_onclick(event)
{
    let row = event.target.closest(".member-conversation-room");

    // not a row
    if (row == null)
    {
        return;
    }

    // open or join
    if (row.classList.contains("member-conversation-room-current"))
    {
        member_conversations__open_channel_chat(row.dataset.channelId);
    }
    else
    {
        member_conversations__join_channel(row.dataset.channelId);
    }
}

/**
 * @brief Shows the chat of a channel, the same way as a press on its conversation tab.
 *        A tab that was just written into the page has no handler yet and gets the standard one.
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
 * @brief Joins a channel the same way as a double click on it in the channel tree: that code skips the
 *        channel the user is already in and asks for the password of a locked channel.
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
        UI.single_channel_doubleclick_join({ stopPropagation: function() {}, target: name_element });
    }
}

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

function member_conversations__show_selected_chat()
{
    // pocket: the people panel lies over the chat, so it closes when a conversation was chosen
    if (document.body.classList.contains("pocket-people-open"))
    {
        document.body.classList.remove("pocket-people-open");
        UI.refresh_member_list_controls();
    }

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
    // Focus the conversation without raising the phone keyboard until the user types.
    let conversation = document.getElementById(g_current_chat_context_id);
    if (conversation != null)
    {
        conversation.tabIndex = -1;
        conversation.focus({ preventScroll: true });
    }
}
