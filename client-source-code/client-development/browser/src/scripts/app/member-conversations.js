// Conversation-list features reuse the member mirror and chat contexts. No separate message store or
// network requests: previews describe only messages already retained in this client.
var member_conversations =
{
    active: false,
    auto_open: false,
    observer: null,
    pending: false
};

function member_conversations__apply_theme()
{
    // Themes opt in through CSS; no theme names or palettes are coupled to this module.
    let style = getComputedStyle(document.documentElement);
    member_conversations.active = style.getPropertyValue("--member-conversations").trim() === "1";
    member_conversations.auto_open = style.getPropertyValue("--member-conversations-auto-open").trim() === "1";
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

    document.getElementById("member-list-header").textContent = "People & chats";
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
            text = channel != null ? channel.textContent : presence != null ? presence.textContent : "Hold to choose a channel";
        }
        // Keep user text as text, including when it resembles markup.
        preview.textContent = text.slice(0, 200);
        timestamp.textContent = time;
    }
    member_conversations__filter_people();
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
