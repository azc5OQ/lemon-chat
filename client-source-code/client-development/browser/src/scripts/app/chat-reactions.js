// This file adds reactions to channel and private chat messages.
// Reactions use the same encrypted connection as the message they refer to.

var chat_reactions =
{
    enabled: false,
    entries: new WeakMap(),
    picker: null,
    selected: null,
    action_state: null,
    close_timer: null,
    suppress_hover: false,
    edits: new WeakMap()
};

/**
 * @brief Gives a message an identifier that stays the same for every recipient.
 * @return string -> random message identifier
 */
function chat_reactions__new_id()
{
    let bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    return Array.from(bytes, function(value)
    {
        return value.toString(16).padStart(2, "0");
    }).join("");
}

/**
 * @brief Adds reaction controls to a displayed text message.
 * @param Element element -> message paragraph
 * @param string id -> identifier inside the encrypted message
 * @param string author -> public key of the message author
 * @param string name -> displayed author name
 * @param object quote -> optional quoted message
 */
function chat_reactions__register(element, id, author, name, quote)
{
    if (!G_HAS_DOM || !element || typeof id !== "string" || !/^[a-f0-9]{32}$/.test(id)
        || typeof author !== "string" || !author || chat_reactions.entries.has(element))
    {
        return;
    }
    let context = element.closest(".chat-context");
    if (!context || chat_replies__find_original(context, { message_id: id, author_public_key: author }))
    {
        return;
    }
    let peer_id = context.id.startsWith("chat-context-pm-") ? Number(context.id.slice(16)) : null;
    let bar = document.createElement("div");
    let state =
    {
        element: element,
        block: element.closest(".single-chat-message") || element,
        id: id,
        author: author,
        own: author === g_rsa_public_key_string && element.classList.contains("local-single-chat-message-content-p"),
        generation: g_worker_connection_generation,
        context: context,
        peer_id: peer_id,
        peer_key: peer_id == null ? null : channel_tree__get_public_key_by_client_id(peer_id),
        votes: new Map(),
        emoji_names: new Map(),
        bar: bar
    };
    element.dataset.chatReactionId = id;
    element.classList.add("chat-reaction-message");
    bar.className = "chat-reactions";
    bar.setAttribute("aria-label", "Message reactions");
    chat_reactions.entries.set(element, state);
    element.after(bar);
    chat_reactions__render(state);
    chat_replies__register(state, name, quote);
    chat_reactions__update_message_access(state);
}

/**
 * @brief Gives an own picture or file the same menu for deleting its message.
 * @param element Delivered attachment displayed in a conversation.
 */
function chat_reactions__register_attachment(element)
{
    if (chat_reactions.entries.has(element))
    {
        return;
    }
    let context = element.closest(".chat-context");
    if (!context)
    {
        return;
    }
    let peer_id = context.id.startsWith("chat-context-pm-") ? Number(context.id.slice(16)) : null;
    let state =
    {
        element: element,
        block: element.closest(".single-chat-message") || element,
        own: true,
        attachment: true,
        generation: g_worker_connection_generation,
        context: context,
        peer_id: peer_id,
        peer_key: peer_id == null ? null : channel_tree__get_public_key_by_client_id(peer_id),
        votes: new Map(),
        emoji_names: new Map(),
        bar: document.createElement("div")
    };
    state.bar.className = "chat-reactions chat-reactions-empty";
    element.classList.add("chat-reaction-message");
    chat_reactions.entries.set(element, state);
    element.after(state.bar);
    chat_reactions__render(state);
}

/**
 * @brief Draws counts and a keyboard accessible choice of reactions.
 * @param object state -> message and its recorded reactions
 */
function chat_reactions__render(state)
{
    if (!state.list)
    {
        state.list = document.createElement("span");
        state.list.className = "chat-reaction-list";
        state.tools = document.createElement("span");
        state.tools.className = "chat-message-tools";
        state.add_button = document.createElement("button");
        state.add_button.type = "button";
        state.add_button.className = "chat-reaction-add";
        state.add_button.title = "Add reaction";
        state.add_button.setAttribute("aria-label", "Add reaction");
        state.add_button.setAttribute("aria-haspopup", "dialog");
        state.add_button.setAttribute("aria-expanded", "false");
        state.add_button.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20 13a8 8 0 1 1-9-9M8 14s1.5 2 4 2 4-2 4-2M8 9h.01M13 9h.01M18 2v6M15 5h6"/></svg>';
        state.add_button.onclick = function()
        {
            chat_reactions__show_choices(state, state.add_button);
        };
        state.tools.appendChild(state.add_button);
        state.bar.append(state.list, state.tools);
        state.tools.hidden = true;
        state.tools.setAttribute("role", "toolbar");
        state.tools.setAttribute("aria-label", "Message actions");
        if (state.own)
        {
            state.edit_button = document.createElement("button");
            state.edit_button.type = "button";
            state.edit_button.className = "chat-message-edit";
            state.edit_button.title = "Edit message";
            state.edit_button.setAttribute("aria-label", "Edit message");
            state.edit_button.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m14 5 5 5M4 20l5-1L20 8a2 2 0 0 0-5-5L4 14z"/></svg>';
            state.edit_button.onclick = function()
            {
                chat_reactions__edit(state);
            };
            if (!state.attachment)
            {
                state.tools.prepend(state.edit_button);
            }
            state.more_button = document.createElement("button");
            state.more_button.type = "button";
            state.more_button.className = "chat-message-more";
            state.more_button.title = "More message actions";
            state.more_button.setAttribute("aria-label", "More message actions");
            state.more_button.setAttribute("aria-haspopup", "menu");
            state.more_button.setAttribute("aria-expanded", "false");
            state.more_button.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/></svg>';
            state.more_button.onclick = function()
            {
                chat_reactions__toggle_more(state);
            };
            state.tools.appendChild(state.more_button);
        }
        state.open_tools = function(event)
        {
            let selection = window.getSelection();
            let touch = event.pointerType === "touch" || (event.sourceCapabilities && event.sourceCapabilities.firesTouchEvents);
            if ((!touch && event.detail > 1) || (selection && !selection.isCollapsed))
            {
                chat_reactions__close_actions(false);
                return;
            }
            if (event.defaultPrevented || event.button !== 0
                || event.target.isContentEditable
                || event.target.closest("a, button, input, textarea, select, label, summary, video, audio, img, [role=button], [data-action]"))
            {
                return;
            }
            chat_reactions__show_actions(state, event);
        };
        state.open_tools_with_keyboard = function(event)
        {
            if ((event.target === state.block || event.target === state.element) && !state.element.isContentEditable
                && (event.key === "Enter" || event.key === " "))
            {
                event.preventDefault();
                chat_reactions__show_actions(state, null);
            }
        };
        state.hover_tools = function(event)
        {
            if (event.type === "pointermove" && (event.movementX || event.movementY))
            {
                chat_reactions.suppress_hover = false;
            }
            if (event.pointerType !== "touch" && !chat_reactions.suppress_hover)
            {
                chat_reactions__show_actions(state, event, true);
            }
        };
        state.leave_tools = function()
        {
            chat_reactions__schedule_close(state);
        };
        state.open_attachment_menu = function(event)
        {
            event.preventDefault();
            chat_reactions__show_actions(state, event);
            if (!state.menu)
            {
                chat_reactions__toggle_more(state);
            }
        };
        state.tools.onpointerenter = function()
        {
            clearTimeout(chat_reactions.close_timer);
        };
        state.tools.onpointerleave = state.leave_tools;
        state.tools.onfocusout = state.leave_tools;
        state.block.addEventListener("pointerenter", state.hover_tools);
        state.block.addEventListener("pointermove", state.hover_tools);
        state.block.addEventListener("pointerleave", state.leave_tools);
        if (state.attachment)
        {
            state.element.addEventListener("contextmenu", state.open_attachment_menu);
        }
        state.block.addEventListener("click", state.open_tools);
        state.block.addEventListener("keydown", state.open_tools_with_keyboard);
        chat_reactions__update_message_access(state);
    }
    let focused = state.list.contains(document.activeElement) ? document.activeElement.dataset.reactionEmoji : null;
    server_emojis__forget_images(state.list);
    state.list.replaceChildren();
    let emojis = Array.from(state.votes.keys()).filter(function(emoji)
    {
        return typeof emoji === "string" && /^server:[a-f0-9]{32}$/.test(emoji);
    });
    for (let emoji of emojis)
    {
        let emoji_name = chat_reactions__emoji_name(emoji, state);
        let votes = state.votes.get(emoji);
        let active = votes ? Array.from(votes.entries()).filter(function(entry)
        {
            return entry[1].active;
        }) : [];
        if (active.length === 0)
        {
            continue;
        }
        let button = document.createElement("button");
        let own = votes.get(g_rsa_public_key_string);
        button.type = "button";
        button.className = "chat-reaction";
        button.dataset.reactionEmoji = emoji;
        let image = document.createElement("span");
        image.className = "chat-reaction-emoji";
        server_emojis__render_image(image, emoji.slice(7), emoji_name);
        button.disabled = !server_emojis__name(emoji.slice(7)) && !(own && own.active);
        let count = document.createElement("span");
        count.className = "chat-reaction-count";
        count.textContent = String(active.length);
        button.append(image, count);
        button.setAttribute("aria-pressed", String(!!own && own.active));
        button.setAttribute("aria-label", emoji_name + ", " + active.length + " reactions");
        let names = active.slice(0, 4).map(function(entry)
        {
            if (entry[0] === g_rsa_public_key_string)
            {
                return "You";
            }
            let client = g_client_list.find(function(candidate)
            {
                return candidate && candidate.public_key === entry[0];
            });
            return client ? channel_tree__get_display_name_by_client_id(client.client_id, client.username) : "A user";
        });
        button.title = names.join(", ") + (active.length > 4 ? " and " + (active.length - 4) + " others" : "")
            + " reacted with " + emoji_name + ". "
            + (own && own.active ? "Click to remove your reaction." : "Click to react.");
        button.onclick = function()
        {
            chat_reactions__send(state, emoji);
        };
        state.list.appendChild(button);
        if (focused === emoji)
        {
            button.focus({ preventScroll: true });
        }
    }
    state.bar.classList.toggle("chat-reactions-empty", !state.list.childElementCount);
}

/**
 * @brief Checks that an own message can still be changed in its original conversation.
 * @param state Message selected for editing or deletion.
 * @return Whether the connection and recipient still match.
 */
function chat_reactions__can_modify(state)
{
    let value = state.element.getAttribute("data-single-chat-message-server-message-id");
    let id = Number(value);
    if (!state.own || !g_is_authenticated || g_worker_connection_ended || state.generation !== g_worker_connection_generation
        || !state.element.isConnected || !value || !/^\d+$/.test(value) || !Number.isSafeInteger(id) || id < 0)
    {
        return false;
    }
    if (state.peer_id != null)
    {
        return !!state.peer_key && state.peer_key === channel_tree__get_public_key_by_client_id(state.peer_id);
    }
    return state.context.id === "chat-context-channel-" + g_current_channel_id;
}

/**
 * @brief Starts editing a message while remembering its original recipient.
 * @param state Own text message selected by the user.
 */
function chat_reactions__edit(state)
{
    if (!state || !chat_reactions__can_modify(state) || chat_reactions.edits.has(state.element))
    {
        return;
    }
    chat_reactions__close_actions(false);
    chat_reactions.edits.set(state.element,
    {
        state: state,
        original: state.element.innerHTML,
        original_text: state.element.textContent,
        message_id: Number(state.element.getAttribute("data-single-chat-message-server-message-id")),
        receiver_type: state.peer_id == null ? "channel" : "user",
        receiver_id: state.peer_id == null ? g_current_channel_id : state.peer_id
    });
    state.element.setAttribute("contenteditable", "true");
    state.element.addEventListener("blur", chat__local_chat_message_onblur);
    state.element.addEventListener("keydown", chat__local_chat_message_onkeydown);
    state.element.focus({ preventScroll: true });
}

/**
 * @brief Deletes the selected own message in its original conversation.
 * @param state Own message selected by the user.
 */
function chat_reactions__delete(state)
{
    if (!chat_reactions__can_modify(state))
    {
        return;
    }
    connection__send_message_object(
    {
        message:
        {
            type: "delete_chat_message_request",
            message_id: Number(state.element.getAttribute("data-single-chat-message-server-message-id")),
            receiver_type: state.peer_id == null ? "channel" : "user",
            receiver_id: state.peer_id == null ? g_current_channel_id : state.peer_id
        }
    });
    chat_reactions__close_actions(false);
}

/**
 * @brief Gives the pointer time to move from the message into its actions.
 * @param state Message that the pointer or keyboard focus left.
 */
function chat_reactions__schedule_close(state)
{
    clearTimeout(chat_reactions.close_timer);
    chat_reactions.close_timer = setTimeout(function()
    {
        if (chat_reactions.action_state === state && !state.pinned && !state.menu && !chat_reactions.picker
            && !state.block.matches(":hover") && !state.tools.matches(":hover")
            && !state.tools.contains(document.activeElement))
        {
            chat_reactions__close_actions(false);
        }
    }, 200);
}

/**
 * @brief Closes the additional actions for a message.
 * @param state Message whose menu is closing.
 * @param restore_focus Whether to focus the More button.
 */
function chat_reactions__close_more(state, restore_focus)
{
    if (state.menu)
    {
        state.menu.remove();
        state.menu = null;
        state.more_button.setAttribute("aria-expanded", "false");
        if (restore_focus)
        {
            state.more_button.focus({ preventScroll: true });
        }
    }
}

/**
 * @brief Opens the additional actions for an own message.
 * @param state Message selected by the user.
 */
function chat_reactions__toggle_more(state)
{
    if (state.menu)
    {
        chat_reactions__close_more(state, true);
        return;
    }
    chat_reactions__close_picker(false);
    let menu = document.createElement("div");
    menu.className = "chat-message-menu";
    menu.setAttribute("role", "menu");
    menu.setAttribute("aria-label", "Message actions");
    if (chat_replies.enabled && state.reply_button)
    {
        state.reply_button.setAttribute("role", "menuitem");
        menu.appendChild(state.reply_button);
    }
    let remove = document.createElement("button");
    remove.type = "button";
    remove.className = "chat-message-delete";
    remove.setAttribute("role", "menuitem");
    remove.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 6h16M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7M14 10v7"/></svg><span>Delete message</span>';
    remove.disabled = !chat_reactions__can_modify(state);
    remove.onclick = function()
    {
        chat_reactions__delete(state);
    };
    menu.appendChild(remove);
    menu.onkeydown = function(event)
    {
        let buttons = Array.from(menu.querySelectorAll("button:not(:disabled)"));
        let index = buttons.indexOf(document.activeElement);
        if (event.key === "ArrowDown" || event.key === "ArrowUp" || event.key === "Home" || event.key === "End")
        {
            event.preventDefault();
            index = event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1
                : (index + (event.key === "ArrowDown" ? 1 : -1) + buttons.length) % buttons.length;
            if (buttons[index])
            {
                buttons[index].focus();
            }
        }
    };
    menu.onfocusout = function(event)
    {
        if (!menu.contains(event.relatedTarget) && !state.tools.contains(event.relatedTarget))
        {
            chat_reactions__close_more(state, false);
        }
    };
    state.menu = menu;
    state.pinned = true;
    state.more_button.setAttribute("aria-expanded", "true");
    document.body.appendChild(menu);
    chat_reactions__position_actions();
    let first = menu.querySelector("button:not(:disabled)");
    if (first)
    {
        first.focus({ preventScroll: true });
    }
}

/**
 * @brief Updates message actions from ownership and the local preferences.
 * @param state Message whose available controls changed.
 */
function chat_reactions__update_message_access(state)
{
    state.add_button.hidden = state.attachment || !chat_reactions.enabled;
    if (state.reply_button)
    {
        state.reply_button.hidden = !chat_replies.enabled;
    }
    if (state.edit_button)
    {
        state.edit_button.disabled = !chat_reactions__can_modify(state);
        state.edit_button.title = state.edit_button.disabled ? "Edit is available after delivery in the current conversation" : "Edit message";
    }
    if (state.own || chat_reactions.enabled || chat_replies.enabled)
    {
        state.block.tabIndex = 0;
    }
    else
    {
        state.block.removeAttribute("tabindex");
    }
}

/**
 * @brief Shows actions at the upper right of a message.
 * @param state Message selected by the user.
 * @param event Click position, or null when opened with the keyboard.
 * @param hover Whether pointer movement opened the toolbar.
 */
function chat_reactions__show_actions(state, event, hover)
{
    let selection = window.getSelection();
    if ((!state.own && !chat_reactions.enabled && !chat_replies.enabled) || !state.element.isConnected
        || state.element.isContentEditable || (selection && !selection.isCollapsed))
    {
        return;
    }
    if (chat_reactions.action_state === state)
    {
        clearTimeout(chat_reactions.close_timer);
        if (hover)
        {
            return;
        }
    }
    else
    {
        if (hover && chat_reactions.action_state && (chat_reactions.action_state.menu || chat_reactions.picker
            || chat_reactions.action_state.tools.contains(document.activeElement)))
        {
            return;
        }
        chat_reactions__close_actions(false);
    }
    state.pinned = !hover;
    chat_reactions.action_state = state;
    chat_reactions__update_message_access(state);
    document.body.appendChild(state.tools);
    state.tools.hidden = false;
    state.block.classList.add("chat-message-actions-visible");
    chat_reactions__position_actions();
    if (!event)
    {
        let button = Array.from(state.tools.children).find(function(control)
        {
            return !control.hidden && !control.disabled;
        });
        if (button)
        {
            button.focus({ preventScroll: true });
        }
    }
}

/**
 * @brief Keeps the open actions beside their message when the chat moves.
 */
function chat_reactions__position_actions()
{
    let state = chat_reactions.action_state;
    if (!state)
    {
        return;
    }
    if (!state.element.isConnected || !state.block.isConnected || !state.block.getClientRects().length)
    {
        chat_reactions__close_actions(false);
        return;
    }
    let rect = state.block.getBoundingClientRect();
    let context_rect = state.context.getBoundingClientRect();
    let chat = state.element.closest("#chat-context-container") || state.context.parentElement;
    let clip = chat.getBoundingClientRect();
    if (rect.bottom <= Math.max(0, clip.top) || rect.top >= Math.min(window.innerHeight, clip.bottom))
    {
        chat_reactions__close_actions(false);
        return;
    }
    let width = state.tools.offsetWidth;
    let height = state.tools.offsetHeight;
    let top = rect.top - height / 2;
    state.tools.style.left = Math.max(8, Math.min(rect.right, context_rect.right, window.innerWidth - 8) - width) + "px";
    state.tools.style.top = Math.max(8, clip.top, Math.min(top, clip.bottom - height - 4, window.innerHeight - height - 8)) + "px";
    if (state.menu)
    {
        let anchor = state.more_button.getBoundingClientRect();
        let menu = state.menu;
        menu.style.left = Math.max(8, Math.min(anchor.right - menu.offsetWidth, window.innerWidth - menu.offsetWidth - 8)) + "px";
        menu.style.top = Math.max(8, anchor.bottom + menu.offsetHeight + 4 <= window.innerHeight - 8
            ? anchor.bottom + 4 : anchor.top - menu.offsetHeight - 4) + "px";
    }
    chat_reactions__position_picker();
}

/**
 * @brief Closes the message actions and the emoji choices opened from them.
 * @param restore_focus Whether the selected message should receive keyboard focus.
 */
function chat_reactions__close_actions(restore_focus)
{
    let state = chat_reactions.action_state;
    let suppress_hover = !!(state && (state.tools.matches(":hover") || (state.menu && state.menu.matches(":hover"))
        || (chat_reactions.picker && chat_reactions.picker.matches(":hover"))));
    clearTimeout(chat_reactions.close_timer);
    chat_reactions__close_picker(false);
    chat_reactions.action_state = null;
    chat_reactions.suppress_hover = suppress_hover;
    if (state)
    {
        chat_reactions__close_more(state, false);
        state.block.classList.remove("chat-message-actions-visible");
        state.tools.hidden = true;
        state.bar.appendChild(state.tools);
        if (restore_focus && state.block.isConnected)
        {
            state.block.focus({ preventScroll: true });
        }
    }
}

/**
 * @brief Places the emoji choices beside the open reaction button.
 */
function chat_reactions__position_picker()
{
    let picker = chat_reactions.picker;
    let state = chat_reactions.selected;
    if (!picker || !state)
    {
        return;
    }
    let rect = state.add_button.getBoundingClientRect();
    let width = picker.offsetWidth;
    let height = picker.offsetHeight;
    picker.style.left = Math.max(8, Math.min(rect.right - width, window.innerWidth - width - 8)) + "px";
    picker.style.top = Math.max(8, Math.min(rect.top >= height + 8 ? rect.top - height - 5 : rect.bottom + 5,
        window.innerHeight - height - 8)) + "px";
}

/**
 * @brief Opens one emoji picker beside the selected message.
 * @param object state -> displayed message
 * @param Element anchor -> button that opened the picker
 */
function chat_reactions__show_choices(state, anchor)
{
    if (!chat_reactions.enabled || !state.element.isConnected)
    {
        return;
    }
    if (chat_reactions.selected === state)
    {
        chat_reactions__close_picker(false);
        return;
    }
    chat_reactions__close_picker(false);
    chat_reactions__close_more(state, false);
    let picker = document.createElement("div");
    picker.className = "chat-reaction-picker";
    picker.setAttribute("role", "dialog");
    picker.setAttribute("aria-label", "Choose a reaction");
    let heading = document.createElement("div");
    heading.className = "chat-reaction-picker-header";
    let title = document.createElement("strong");
    title.textContent = "Add a reaction";
    let close = document.createElement("button");
    close.type = "button";
    close.className = "chat-reaction-picker-close";
    close.textContent = "×";
    close.setAttribute("aria-label", "Close reactions");
    close.onclick = function()
    {
        chat_reactions__close_picker(true);
    };
    heading.append(title, close);
    let search = document.createElement("input");
    search.type = "search";
    search.placeholder = "Find an emoji";
    search.setAttribute("aria-label", "Find an emoji");
    let choices = document.createElement("div");
    choices.className = "chat-reaction-choices";
    for (let entry of server_emojis__get_entries())
    {
        let button = document.createElement("button");
        let image = document.createElement("span");
        button.type = "button";
        button.title = ":" + entry.name + ":";
        button.setAttribute("aria-label", entry.name);
        button.dataset.emojiName = entry.name;
        server_emojis__render_image(image, entry.id, entry.name);
        button.appendChild(image);
        button.onclick = function(event)
        {
            chat_reactions__send(state, "server:" + entry.id);
            if (!event.shiftKey)
            {
                chat_reactions__close_actions(true);
            }
        };
        choices.appendChild(button);
    }
    let empty = document.createElement("p");
    empty.className = "chat-reaction-empty";
    empty.textContent = choices.childElementCount ? "No matching emoji" : "This server has no emojis.";
    empty.hidden = choices.childElementCount > 0;
    search.oninput = function()
    {
        let matches = 0;
        for (let button of choices.children)
        {
            button.hidden = !button.dataset.emojiName.includes(search.value.trim().toLowerCase());
            matches += button.hidden ? 0 : 1;
        }
        empty.hidden = matches !== 0;
    };
    picker.append(heading, search, choices, empty);
    document.body.appendChild(picker);
    chat_reactions.picker = picker;
    chat_reactions.selected = state;
    anchor.setAttribute("aria-expanded", "true");
    chat_reactions__position_picker();
    search.focus({ preventScroll: true });
}

/**
 * @brief Closes the emoji picker and optionally returns focus to its message.
 * @param restore_focus -> whether the Add reaction button should receive focus
 */
function chat_reactions__close_picker(restore_focus)
{
    let state = chat_reactions.selected;
    if (chat_reactions.picker)
    {
        server_emojis__forget_images(chat_reactions.picker);
        chat_reactions.picker.remove();
    }
    chat_reactions.picker = null;
    chat_reactions.selected = null;
    if (state && state.add_button)
    {
        state.add_button.setAttribute("aria-expanded", "false");
        if (restore_focus && state.add_button.isConnected)
        {
            state.add_button.focus({ preventScroll: true });
        }
    }
}

/**
 * @brief Sends a reaction through the existing encrypted chat message route.
 * @param object state -> message being reacted to
 * @param string emoji -> chosen reaction
 */
function chat_reactions__send(state, emoji)
{
    if (!chat_reactions.enabled || !g_is_authenticated || !state.element.isConnected)
    {
        return;
    }
    let request = null;
    if (state.peer_id != null)
    {
        if (!state.peer_key || state.peer_key !== channel_tree__get_public_key_by_client_id(state.peer_id))
        {
            utils__custom_alert("This user is no longer connected to this conversation.");
            return;
        }
        request =
        {
            type: "mainthread__create_direct_chat_message",
            receiver_public_key: state.peer_key,
            chat_message_receiver_id: state.peer_id
        };
    }
    else
    {
        if (state.context.id !== "chat-context-channel-" + g_current_channel_id || !g_current_channel_keys)
        {
            utils__custom_alert("Join this channel and wait for its keys before reacting.");
            return;
        }
        request =
        {
            type: "mainthread__create_channel_chat_message",
            current_channel_id: g_current_channel_id,
            current_channel_keys: g_current_channel_keys
        };
    }
    let votes = state.votes.get(emoji);
    let own = votes && votes.get(g_rsa_public_key_string);
    if (typeof emoji !== "string" || !/^server:[a-f0-9]{32}$/.test(emoji)
        || (!server_emojis__name(emoji.slice(7)) && !(own && own.active)))
    {
        return;
    }
    let payload =
    {
        type: "chat_reaction",
        message_id: state.id,
        author_public_key: state.author,
        emoji: emoji,
        emoji_name: chat_reactions__emoji_name(emoji, state),
        active: !own || !own.active
    };
    request.local_message_id = g_local_message_id++;
    if (state.peer_id != null)
    {
        request.chat_message_value = JSON.stringify(payload);
    }
    else
    {
        request.chat_message_to_send_value = JSON.stringify(payload);
    }
    g_data_processing_worker.postMessage(request);
    chat_reactions__apply(state, payload, g_rsa_public_key_string, request.local_message_id);
}

/**
 * @brief Records each person's current reaction without counting duplicates.
 * @param object state -> displayed message
 * @param object payload -> validated reaction
 * @param string actor -> sender public key from the client list
 * @param number sequence -> message order supplied by the server
 */
function chat_reactions__apply(state, payload, actor, sequence)
{
    if (typeof payload.emoji !== "string" || !/^server:[a-f0-9]{32}$/.test(payload.emoji)
        || (!server_emojis__name(payload.emoji.slice(7)) && (!state.votes.has(payload.emoji) || payload.active)))
    {
        return;
    }
    if (!state.votes.has(payload.emoji) && state.votes.size >= 256)
    {
        return;
    }
    if (typeof payload.emoji_name === "string" && /^[a-z0-9_]{1,32}$/.test(payload.emoji_name))
    {
        state.emoji_names.set(payload.emoji, payload.emoji_name);
    }
    let votes = state.votes.get(payload.emoji);
    if (!votes)
    {
        votes = new Map();
        state.votes.set(payload.emoji, votes);
    }
    let previous = votes.get(actor);
    if (previous && previous.sequence >= sequence)
    {
        return;
    }
    votes.set(actor, { active: payload.active, sequence: sequence });
    if (chat_reactions.enabled)
    {
        chat_reactions__render(state);
    }
}

/**
 * @brief Handles reactions before the normal message handler adds text or alerts.
 * @param object payload -> decrypted chat contents
 * @param object message -> sender and destination supplied by the server
 * @param boolean direct -> true for private chat
 * @return boolean -> true when this payload is a reaction
 */
function chat_reactions__receive(payload, message, direct)
{
    if (!payload || payload.type !== "chat_reaction")
    {
        return false;
    }
    if (!G_HAS_DOM || message.sender_id == g_local_client_id
        || typeof payload.message_id !== "string" || !/^[a-f0-9]{32}$/.test(payload.message_id)
        || typeof payload.emoji !== "string" || !/^server:[a-f0-9]{32}$/.test(payload.emoji)
        || typeof payload.emoji_name !== "string" || !/^[a-z0-9_]{1,32}$/.test(payload.emoji_name)
        || typeof payload.active !== "boolean"
        || !Number.isSafeInteger(message.server_chat_message_id))
    {
        return true;
    }
    let sender = channel_tree__get_client_by_client_id(message.sender_id);
    let actor = channel_tree__get_public_key_by_client_id(message.sender_id);
    if (!sender || sender.is_ignored_by_local_client || !actor)
    {
        return true;
    }
    let context = document.getElementById(direct ? "chat-context-pm-" + message.sender_id : "chat-context-channel-" + message.channel_id);
    let element = chat_replies__find_original(context, payload);
    let state = element && chat_reactions.entries.get(element);
    if (state && state.author === payload.author_public_key && (!direct || state.peer_key === actor))
    {
        chat_reactions__apply(state, payload, actor, message.server_chat_message_id);
    }
    return true;
}

/**
 * @brief Uses the server's current emoji name or the name kept with an older reaction.
 * @param emoji -> server emoji identifier
 * @param state -> message whose reactions contain a saved name
 * @return string -> readable emoji name
 */
function chat_reactions__emoji_name(emoji, state)
{
    return server_emojis__name(emoji.slice(7)) || state.emoji_names.get(emoji) || "deleted_emoji";
}

/**
 * @brief Updates existing reactions and an open picker after server emojis change.
 */
function chat_reactions__refresh_server_emojis()
{
    if (!G_HAS_DOM || !chat_reactions.enabled)
    {
        return;
    }
    for (let element of document.querySelectorAll(".chat-reaction-message"))
    {
        let state = chat_reactions.entries.get(element);
        if (state)
        {
            chat_reactions__render(state);
        }
    }
    if (chat_reactions.selected)
    {
        let state = chat_reactions.selected;
        let search = chat_reactions.picker.querySelector('input[type="search"]').value;
        chat_reactions__close_picker(false);
        chat_reactions__show_choices(state, state.add_button);
        let input = chat_reactions.picker.querySelector('input[type="search"]');
        input.value = search;
        input.dispatchEvent(new Event("input"));
    }
}

/**
 * @brief Removes reactions when the message is deleted or trimmed from history.
 * @param Element element -> message being removed
 * @param is_deleted Whether the server requested deletion of the message.
 */
function chat_reactions__forget(element, is_deleted)
{
    let state = element && chat_reactions.entries.get(element);
    if (element && element.dataset.chatMessageId)
    {
        chat_replies__forget(state ||
        {
            element: element,
            context: element.closest(".chat-context"),
            id: element.dataset.chatMessageId
        }, is_deleted === true);
    }
    if (state)
    {
        if (chat_reactions.action_state === state)
        {
            chat_reactions__close_actions(false);
        }
        else if (chat_reactions.selected === state)
        {
            chat_reactions__close_picker(false);
        }
        let context = element.closest(".chat-context");
        let pending = context && chat_replies.pending.get(context);
        if (pending && pending.state === state)
        {
            chat_replies.pending.delete(context);
            chat_replies__refresh();
        }
        state.block.removeEventListener("click", state.open_tools);
        state.block.removeEventListener("keydown", state.open_tools_with_keyboard);
        state.block.removeEventListener("pointerenter", state.hover_tools);
        state.block.removeEventListener("pointermove", state.hover_tools);
        state.block.removeEventListener("pointerleave", state.leave_tools);
        state.element.removeEventListener("contextmenu", state.open_attachment_menu);
        if (chat_reactions.edits.has(element))
        {
            state.element.removeEventListener("blur", chat__local_chat_message_onblur);
            state.element.removeEventListener("keydown", chat__local_chat_message_onkeydown);
            state.element.removeAttribute("contenteditable");
            chat_reactions.edits.delete(element);
        }
        state.block.removeAttribute("tabindex");
        server_emojis__forget_images(state.bar);
        state.bar.remove();
        element.removeAttribute("data-chat-reaction-id");
        element.classList.remove("chat-reaction-message");
        chat_reactions.entries.delete(element);
    }
}

/**
 * @brief Restores the local preference and connects its checkbox.
 */
function chat_reactions__setup()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let checkbox = document.getElementById("local-settings-chat-reactions");
    chat_reactions.enabled = utils__storage_get("lemon_chat_reactions") === "1";
    checkbox.checked = chat_reactions.enabled;
    document.body.classList.toggle("chat-reactions-disabled", !chat_reactions.enabled);
    checkbox.onchange = function()
    {
        chat_reactions.enabled = checkbox.checked;
        utils__storage_set("lemon_chat_reactions", checkbox.checked ? "1" : "0");
        document.body.classList.toggle("chat-reactions-disabled", !checkbox.checked);
        chat_reactions__close_actions(false);
        for (let element of document.querySelectorAll("[data-chat-reaction-id]"))
        {
            let state = chat_reactions.entries.get(element);
            if (state)
            {
                chat_reactions__update_message_access(state);
                if (checkbox.checked)
                {
                    chat_reactions__render(state);
                }
            }
        }
    };
    document.addEventListener("click", function(event)
    {
        let state = chat_reactions.action_state;
        if (state && !state.block.contains(event.target) && !state.tools.contains(event.target)
            && (!state.menu || !state.menu.contains(event.target))
            && (!chat_reactions.picker || !chat_reactions.picker.contains(event.target)))
        {
            chat_reactions__close_actions(false);
        }
    });
    document.addEventListener("keydown", function(event)
    {
        if (event.key === "Escape" && chat_reactions.action_state)
        {
            if (chat_reactions.action_state.menu)
            {
                chat_reactions__close_more(chat_reactions.action_state, true);
                return;
            }
            chat_reactions__close_actions(true);
        }
    });
    document.addEventListener("selectionchange", function()
    {
        let selection = window.getSelection();
        if (chat_reactions.action_state && selection && !selection.isCollapsed
            && (!chat_reactions.picker || !chat_reactions.picker.contains(selection.anchorNode)))
        {
            chat_reactions__close_actions(false);
        }
    });
    document.addEventListener("scroll", function(event)
    {
        if (!chat_reactions.picker || !chat_reactions.picker.contains(event.target))
        {
            chat_reactions__position_actions();
        }
    }, true);
    window.addEventListener("resize", chat_reactions__position_actions);
}
