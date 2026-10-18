// This file limits how many messages each chat keeps on this device and lets
// the user search those messages without sending a search request to the server.

var chat_history =
{
    enabled: false,
    limit: 1000,
    search_enabled: true,
    observer: null,
    timer: null,
    changed: new Set(),
    matches: [],
    selected: -1,
    search_context: null,
    previous_focus: null
};

/**
 * @brief Finds individual messages, including separate lines from the same sender.
 * @param Element context -> one channel or private chat
 * @return Array -> messages in display order, including pictures, file cards and notices
 */
function chat_history__entries(context)
{
    return Array.from(context.querySelectorAll(
        ".single-chat-message-content > .single-chat-message-content-p, "
        + ".single-chat-message-content > .chat-picture-img, "
        + ".single-chat-message-content > .chat-file-card, :scope > .single-server-message"));
}

/**
 * @brief Removes the oldest entries and releases the downloaded files they displayed.
 * @param Element context -> chat whose limit should be applied
 */
function chat_history__trim(context)
{
    let entries = chat_history__entries(context);
    let entry = null;
    let block = null;
    let progress = null;
    let key = null;
    let message_id = null;
    let context_state = null;

    for (let i = 0; i < entries.length - chat_history.limit; i++)
    {
        entry = entries[i];
        block = entry.closest(".single-chat-message");
        key = entry.getAttribute("data-chat-file-key");
        message_id = entry.getAttribute("data-single-chat-message-server-message-id");
        if (key != null)
        {
            delete g_chat_files_by_message_id[key];
            delete g_chat_file_transfers[key];
        }
        if (message_id != null)
        {
            delete g_chat_message_author_public_keys[message_id];
        }
        if (entry.classList.contains("chat-picture-img"))
        {
            progress = entry.previousElementSibling;
            if (progress != null && progress.classList.contains("chat-picture-progress"))
            {
                delete g_chat_file_transfers[progress.getAttribute("data-chat-file-key")];
                progress.remove();
            }
        }
        if (entry.classList.contains("single-chat-message-content-p"))
        {
            link_previews__forget(entry);
        }
        entry.remove();
        if (block != null && chat_history__entries(block).length === 0)
        {
            block.remove();
        }
    }
    if (context.querySelector(".single-chat-message") == null)
    {
        context_state = g_chat_context_array.find(item => item.chat_context_id === context.id);
        if (context_state != null)
        {
            context_state.last_known_message_sender_username = "";
        }
    }
}

/**
 * @brief Applies changes once for messages arriving together, including messages in hidden chats.
 */
function chat_history__refresh()
{
    let container = document.getElementById("chat-context-container");
    let context = document.getElementById(g_current_chat_context_id);
    let search_changed = context !== chat_history.search_context || chat_history.changed.has(context);

    clearTimeout(chat_history.timer);
    chat_history.timer = null;
    chat_history.observer.disconnect();
    if (chat_history.enabled)
    {
        for (let changed of chat_history.changed)
        {
            if (changed.isConnected)
            {
                chat_history__trim(changed);
            }
        }
    }
    chat_history.changed.clear();
    if (chat_history__is_search_open() && search_changed)
    {
        chat_history__search(false);
    }
    chat_history.observer.observe(container,
    {
        childList: true,
        subtree: true,
        characterData: true,
        attributes: true,
        attributeFilter: ["style"]
    });
}

/**
 * @brief Reports whether the user is searching instead of following new messages.
 * @return boolean -> true while the search box is visible
 */
function chat_history__is_search_open()
{
    let bar = typeof document === "undefined" ? null : document.getElementById("chat-search");
    return bar != null && !bar.hidden;
}

/**
 * @brief Finds literal text in the current chat, including words split by text formatting.
 * @param boolean reveal -> scroll to the selected match when the user changes the search
 */
function chat_history__search(reveal)
{
    let query = document.getElementById("chat-search-input").value;
    let context = document.getElementById(g_current_chat_context_id);
    let previous = chat_history.matches[chat_history.selected];
    let pattern = null;
    let walker = null;
    let nodes = null;
    let node = null;
    let text = "";
    let match = null;
    let range = null;
    let start = null;
    let end = null;
    let offset = 0;

    chat_history.matches = [];
    chat_history.selected = -1;
    chat_history.search_context = context;
    if (context != null && query.length > 0)
    {
        pattern = new RegExp(query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "giu");
        for (let entry of chat_history__entries(context))
        {
            nodes = [];
            text = "";
            walker = document.createTreeWalker(entry.querySelector(".chat-file-name") || entry, NodeFilter.SHOW_TEXT);
            while ((node = walker.nextNode()) != null)
            {
                nodes.push({ node: node, start: text.length, end: text.length + node.length });
                text += node.data;
            }
            pattern.lastIndex = 0;
            offset = 0;
            while ((match = pattern.exec(text)) != null)
            {
                while (offset < nodes.length && nodes[offset].end <= match.index)
                {
                    offset++;
                }
                start = nodes[offset];
                end = nodes.find(part => part.end >= match.index + match[0].length);
                range = document.createRange();
                range.setStart(start.node, match.index - start.start);
                range.setEnd(end.node, match.index + match[0].length - end.start);
                if (previous != null && previous.entry === entry && previous.offset === match.index)
                {
                    chat_history.selected = chat_history.matches.length;
                }
                chat_history.matches.push({ entry: entry, range: range, offset: match.index });
            }
        }
    }
    if (chat_history.selected < 0 && chat_history.matches.length > 0)
    {
        chat_history.selected = 0;
    }
    chat_history__show_match(reveal);
}

/**
 * @brief Highlights matches without changing message HTML or removing its click handlers.
 * @param boolean reveal -> move the chat scroll position to the selected result
 */
function chat_history__show_match(reveal)
{
    let selected = chat_history.matches[chat_history.selected];
    let container = document.getElementById("chat-context-container");
    let box = null;
    let bounds = null;
    let bar_bounds = null;
    let top = 0;
    let highlights = null;

    for (let entry of document.querySelectorAll(".chat-search-selected"))
    {
        entry.classList.remove("chat-search-selected");
    }
    if (typeof CSS !== "undefined" && CSS.highlights && typeof Highlight !== "undefined")
    {
        highlights = new Highlight();
        for (let match of chat_history.matches)
        {
            highlights.add(match.range);
        }
        CSS.highlights.set("chat-search-results", highlights);
        CSS.highlights.set("chat-search-current", new Highlight(...(selected ? [selected.range] : [])));
    }
    document.getElementById("chat-search-count").textContent = selected
        ? (chat_history.selected + 1) + " / " + chat_history.matches.length : "0 matches";
    document.getElementById("chat-search-previous").disabled = !selected;
    document.getElementById("chat-search-next").disabled = !selected;
    if (selected)
    {
        selected.entry.classList.add("chat-search-selected");
        if (reveal)
        {
            box = selected.range.getBoundingClientRect();
            bounds = container.getBoundingClientRect();
            bar_bounds = document.getElementById("chat-search").getBoundingClientRect();
            top = Math.max(bounds.top, bar_bounds.bottom) + 12;
            if (box.top < top || box.bottom > bounds.bottom)
            {
                container.scrollTop += box.top - top;
            }
        }
    }
}

/**
 * @brief Selects the previous or next occurrence, wrapping at either end of the chat.
 * @param number direction -> -1 for previous, 1 for next
 */
function chat_history__step(direction)
{
    chat_history__search(false);
    if (chat_history.matches.length > 0)
    {
        chat_history.selected = (chat_history.selected + direction + chat_history.matches.length) % chat_history.matches.length;
        chat_history__show_match(true);
    }
}

/**
 * @brief Closes search, releases its message references and returns keyboard focus.
 * @param boolean restore_focus -> false when closing from local settings
 */
function chat_history__close_search(restore_focus)
{
    document.getElementById("chat-search").hidden = true;
    chat_history.matches = [];
    chat_history.selected = -1;
    chat_history__show_match(false);
    if (restore_focus !== false && chat_history.previous_focus != null && chat_history.previous_focus.isConnected)
    {
        chat_history.previous_focus.focus({ preventScroll: true });
    }
    chat_history.previous_focus = null;
}

/**
 * @brief Restores the local limit and connects the search box and keyboard shortcuts.
 */
function chat_history__setup()
{
    let checkbox = document.getElementById("local-settings-limit-chat-messages");
    let number = document.getElementById("local-settings-chat-message-limit");
    let row = document.getElementById("local-settings-chat-message-limit-row");
    let bar = document.getElementById("chat-search");
    let input = document.getElementById("chat-search-input");
    let saved = Number(utils__storage_get("lemon_chat_message_limit"));
    let search_checkbox = document.getElementById("local-settings-chat-search");

    chat_history.enabled = utils__storage_get("lemon_limit_chat_messages") === "1";
    chat_history.limit = Number.isSafeInteger(saved) && saved > 0 ? saved : 1000;
    checkbox.checked = chat_history.enabled;
    number.value = chat_history.limit;
    number.disabled = !chat_history.enabled;
    row.hidden = !chat_history.enabled;
    chat_history.search_enabled = utils__storage_get("lemon_chat_search") !== "0";
    search_checkbox.checked = chat_history.search_enabled;
    search_checkbox.onchange = function()
    {
        chat_history.search_enabled = this.checked;
        utils__storage_set("lemon_chat_search", this.checked ? "1" : "0");
        if (!this.checked)
        {
            chat_history__close_search(false);
        }
    };
    chat_history.observer = new MutationObserver(function(records)
    {
        let element = null;
        let context = null;

        if (!chat_history.enabled && !chat_history__is_search_open())
        {
            return;
        }
        for (let record of records)
        {
            element = record.target.nodeType === 1 ? record.target : record.target.parentElement;
            if (record.type === "attributes" && !element.classList.contains("chat-context"))
            {
                continue;
            }
            context = element.closest(".chat-context");
            if (context != null)
            {
                chat_history.changed.add(context);
            }
            for (let added of record.addedNodes)
            {
                if (added.nodeType === 1 && added.classList.contains("chat-context"))
                {
                    chat_history.changed.add(added);
                }
            }
        }
        if (chat_history.timer == null)
        {
            chat_history.timer = setTimeout(chat_history__refresh, 50);
        }
    });
    checkbox.onchange = function()
    {
        chat_history.enabled = this.checked;
        utils__storage_set("lemon_limit_chat_messages", this.checked ? "1" : "0");
        number.disabled = !this.checked;
        row.hidden = !this.checked;
        chat_history.changed = new Set(document.querySelectorAll(".chat-context"));
        chat_history__refresh();
    };
    number.oninput = function()
    {
        this.setCustomValidity("");
    };
    number.onchange = function()
    {
        let value = Number(this.value);

        if (!this.validity.valid || !Number.isSafeInteger(value) || value < 1)
        {
            this.setCustomValidity("Enter a whole number of at least 1.");
            this.reportValidity();
            return;
        }
        this.setCustomValidity("");
        chat_history.limit = value;
        utils__storage_set("lemon_chat_message_limit", String(value));
        chat_history.changed = new Set(document.querySelectorAll(".chat-context"));
        chat_history__refresh();
    };
    number.onblur = function()
    {
        if (this.value !== String(chat_history.limit))
        {
            this.onchange();
        }
    };
    input.oninput = function()
    {
        chat_history.matches = [];
        chat_history__search(true);
    };
    bar.addEventListener("keydown", function(event)
    {
        event.stopPropagation();
        if (event.key === "Enter")
        {
            event.preventDefault();
            chat_history__step(event.shiftKey ? -1 : 1);
        }
    });
    bar.addEventListener("keyup", function(event)
    {
        event.stopPropagation();
    });
    document.getElementById("chat-search-previous").onclick = function()
    {
        chat_history__step(-1);
    };
    document.getElementById("chat-search-next").onclick = function()
    {
        chat_history__step(1);
    };
    document.getElementById("chat-search-close").onclick = chat_history__close_search;
    document.addEventListener("keydown", function(event)
    {
        let window = document.getElementById("chat-window-container");
        let target = event.target;

        if (event.key === "Escape" && chat_history__is_search_open())
        {
            event.preventDefault();
            event.stopImmediatePropagation();
            chat_history__close_search();
        }
        else if (chat_history.search_enabled && (event.ctrlKey || event.metaKey) && !event.altKey && event.key.toLowerCase() === "f"
            && window.getClientRects().length > 0)
        {
            if (target.closest("input, textarea, [contenteditable='true']") != null
                && !window.contains(target) && target.id !== "chat-input-container-text-input")
            {
                return;
            }
            event.preventDefault();
            event.stopImmediatePropagation();
            if (!chat_history__is_search_open())
            {
                chat_history.previous_focus = document.activeElement;
            }
            bar.hidden = false;
            chat_history__search(true);
            input.focus({ preventScroll: true });
            input.select();
        }
    }, true);
    chat_history.changed = new Set(document.querySelectorAll(".chat-context"));
    chat_history__refresh();
}
