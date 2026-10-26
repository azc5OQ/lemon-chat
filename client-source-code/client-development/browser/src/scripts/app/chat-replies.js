// This file adds quoted replies to channel and private messages.
// The quote is sent inside the encrypted message along with the reply text.

var chat_replies =
{
    enabled: false,
    pending: new WeakMap(),
    messages: new WeakMap(),
    references: new WeakMap()
};

/**
 * @brief Finds the quoted message by its identifier and author in the same conversation.
 * @param context Conversation containing the reply.
 * @param quote Identifier and author saved with the reply.
 * @return Original message element or null when it is no longer stored.
 */
function chat_replies__find_original(context, quote)
{
    if (!context)
    {
        return null;
    }
    for (let element of context.querySelectorAll('[data-chat-message-id="' + quote.message_id + '"]'))
    {
        let message = chat_replies.messages.get(element);
        if (message && message.author === quote.author_public_key)
        {
            return element;
        }
    }
    return null;
}

/**
 * @brief Shows whether the quoted message is still available without changing its saved text.
 * @param block Quoted preview displayed above a reply.
 */
function chat_replies__update_reference(block)
{
    let reference = chat_replies.references.get(block);
    let original = chat_replies__find_original(block.closest(".chat-context"), reference.quote);
    let message = original && chat_replies.messages.get(original);
    reference.is_deleted = reference.is_deleted || (!!message && message.deleted);
    let deleted = reference.is_deleted;
    let unavailable = !original;
    reference.text.textContent = deleted ? "Original message deleted" : reference.quote.text;
    reference.status.hidden = !unavailable;
    reference.status.textContent = unavailable ? "Original message is not in this chat history" : "";
    block.classList.toggle("chat-reply-unavailable", unavailable);
    block.title = unavailable ? "The quoted text is saved with this reply" : "Show the original message";
    block.setAttribute("aria-label", "Reply to " + reference.quote.author_name + ": "
        + reference.text.textContent + (unavailable ? ". " + reference.status.textContent : ". Show the original message"));
}

/**
 * @brief Updates previews when their original message arrives or is removed.
 * @param context Conversation containing the changed message.
 * @param id Identifier of the changed message.
 */
function chat_replies__update_references(context, id)
{
    if (!context)
    {
        return;
    }
    for (let block of context.querySelectorAll('[data-chat-reply-id="' + id + '"]'))
    {
        chat_replies__update_reference(block);
    }
}

/**
 * @brief Keeps deleted messages reachable and updates quotes when history is trimmed.
 * @param state Message whose reaction controls are being removed.
 * @param is_deleted Whether the message was deleted rather than removed from local history.
 */
function chat_replies__forget(state, is_deleted)
{
    let message = chat_replies.messages.get(state.element);
    if (state.quote_element)
    {
        chat_replies.references.delete(state.quote_element);
        state.quote_element.remove();
    }
    if (message && is_deleted)
    {
        message.deleted = true;
    }
    else
    {
        chat_replies.messages.delete(state.element);
        state.element.removeAttribute("data-chat-message-id");
    }
    chat_replies__update_references(state.context, state.id);
}

/**
 * @brief Checks a received quote and limits the text shown in it.
 * @param object quote -> quote from the encrypted message
 * @return object -> checked quote or null
 */
function chat_replies__validate(quote)
{
    if (!quote || typeof quote.message_id !== "string" || !/^[a-f0-9]{32}$/.test(quote.message_id)
        || typeof quote.author_public_key !== "string" || !quote.author_public_key || quote.author_public_key.length > 16384
        || typeof quote.author_name !== "string" || typeof quote.text !== "string")
    {
        return null;
    }
    let result =
    {
        message_id: quote.message_id,
        author_public_key: quote.author_public_key,
        author_name: quote.author_name.slice(0, 128),
        text: quote.text.slice(0, 320)
    };
    return result;
}

/**
 * @brief Adds the Reply button and displays the quote attached to a message.
 * @param object state -> displayed message and its author
 * @param string name -> author name supplied with the message
 * @param object quote -> optional quote inside the encrypted message
 */
function chat_replies__register(state, name, quote)
{
    state.author_name = typeof name === "string" ? name.slice(0, 128) : "Unknown user";
    state.element.dataset.chatMessageId = state.id;
    chat_replies.messages.set(state.element,
    {
        author: state.author,
        deleted: false
    });
    chat_replies__update_references(state.context, state.id);
    let button = document.createElement("button");
    button.type = "button";
    button.className = "chat-reply-button";
    button.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m9 5-6 6 6 6M3 11h10a7 7 0 0 1 7 7"/></svg><span>Reply</span>';
    button.title = "Reply to " + state.author_name;
    button.onclick = function()
    {
        let input = document.getElementById("chat-input-container-text-input");
        if (!chat_replies.enabled || !state.element.isConnected || input.disabled
            || state.element.closest(".chat-context").id !== g_current_chat_context_id)
        {
            return;
        }
        chat_replies.pending.set(state.element.closest(".chat-context"),
        {
            state: state,
            generation: g_worker_connection_generation
        });
        chat_reactions__close_actions(false);
        chat_replies__refresh();
        input.focus();
    };
    state.reply_button = button;
    button.setAttribute("aria-label", "Reply to " + state.author_name);
    if (!state.own)
    {
        state.tools.prepend(button);
    }
    quote = chat_replies__validate(quote);
    if (!quote)
    {
        return;
    }
    let block = document.createElement("button");
    block.type = "button";
    block.className = "chat-reply-quote";
    block.dataset.chatReplyId = quote.message_id;
    let arrow = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    arrow.setAttribute("viewBox", "0 0 20 20");
    arrow.setAttribute("aria-hidden", "true");
    arrow.innerHTML = '<path d="M17 16H9a5 5 0 0 1-5-5V3m-3 4 3-4 4 4"/>';
    let author = document.createElement("strong");
    author.textContent = quote.author_name;
    author.title = quote.author_name;
    let content = document.createElement("span");
    content.className = "chat-reply-content";
    let text = document.createElement("span");
    text.className = "chat-reply-text";
    let status = document.createElement("span");
    status.className = "chat-reply-status";
    status.setAttribute("role", "status");
    content.append(author, text, status);
    block.append(arrow, content);
    chat_replies.references.set(block,
    {
        quote: quote,
        text: text,
        status: status
    });
    block.onclick = function()
    {
        let context = state.element.closest(".chat-context");
        let original = chat_replies__find_original(context, quote);
        chat_replies__update_reference(block);
        if (!original)
        {
            return;
        }
        let message = chat_replies.messages.get(original);
        clearTimeout(message.highlight_timer);
        original.scrollIntoView({ block: "center", behavior: "auto" });
        original.classList.add("chat-reply-highlight");
        original.tabIndex = -1;
        original.focus({ preventScroll: true });
        if (!message.deleted)
        {
            chat_reactions__update_message_access(chat_reactions.entries.get(original));
        }
        message.highlight_timer = setTimeout(function()
        {
            original.classList.remove("chat-reply-highlight");
        }, 1600);
    };
    state.quote_element = block;
    state.element.before(block);
    chat_replies__update_reference(block);
}

/**
 * @brief Takes the selected quote when sending a text message in this chat.
 * @param Element context -> chat receiving the new message
 * @return object -> quote to encrypt with the message or null
 */
function chat_replies__take(context)
{
    if (!G_HAS_DOM || !context)
    {
        return null;
    }
    let pending = chat_replies.pending.get(context);
    chat_replies.pending.delete(context);
    chat_replies__refresh();
    if (!chat_replies.enabled || !pending || pending.generation !== g_worker_connection_generation
        || !pending.state.element.isConnected)
    {
        return null;
    }
    let result =
    {
        message_id: pending.state.id,
        author_public_key: pending.state.author,
        author_name: pending.state.author_name,
        text: pending.state.element.textContent.slice(0, 320)
    };
    return result;
}

/**
 * @brief Shows the selected quote for the chat currently open in the composer.
 */
function chat_replies__refresh()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let preview = document.getElementById("chat-reply-preview");
    if (!preview)
    {
        return;
    }
    let context = document.getElementById(g_current_chat_context_id);
    let pending = context && chat_replies.pending.get(context);
    let visible = chat_replies.enabled && pending && pending.generation === g_worker_connection_generation
        && pending.state.element.isConnected;
    preview.hidden = !visible;
    if (visible)
    {
        document.getElementById("chat-reply-author").textContent = "Reply to " + pending.state.author_name;
        document.getElementById("chat-reply-author").title = pending.state.author_name;
        document.getElementById("chat-reply-text").textContent = pending.state.element.textContent.slice(0, 320);
    }
}

/**
 * @brief Restores the local reply preference and connects the cancel button.
 */
function chat_replies__setup()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let checkbox = document.getElementById("local-settings-chat-replies");
    chat_replies.enabled = utils__storage_get("lemon_chat_replies", "1") === "1";
    checkbox.checked = chat_replies.enabled;
    document.body.classList.toggle("chat-replies-disabled", !chat_replies.enabled);
    checkbox.onchange = function()
    {
        chat_replies.enabled = checkbox.checked;
        chat_reactions__close_actions(false);
        for (let element of document.querySelectorAll("[data-chat-reaction-id]"))
        {
            let state = chat_reactions.entries.get(element);
            if (state)
            {
                chat_reactions__update_message_access(state);
            }
        }
        utils__storage_set("lemon_chat_replies", checkbox.checked ? "1" : "0");
        document.body.classList.toggle("chat-replies-disabled", !checkbox.checked);
        if (!checkbox.checked)
        {
            chat_replies.pending = new WeakMap();
        }
        chat_replies__refresh();
    };
    document.getElementById("chat-reply-cancel").onclick = function()
    {
        let context = document.getElementById(g_current_chat_context_id);
        if (context)
        {
            chat_replies.pending.delete(context);
        }
        chat_replies__refresh();
        document.getElementById("chat-input-container-text-input").focus();
    };
}
