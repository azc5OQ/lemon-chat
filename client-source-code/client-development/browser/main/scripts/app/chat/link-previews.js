// this file adds a small card below a chat message that contains a website link
// the client sends the URL to its chat server, which looks up the page title,
// description and picture. these requests and replies use the same metadata
// encryption as channel names and other messages between client and server
var g_link_previews =
{
    observer: null,
    timer: null,
    sequence: 0,
    messages: new Map(),
    urls: new Map()
};

/**
 * @brief normalizes a bounded HTTP URL without credentials or a fragment
 *
 * @param string value -> URL to validate
 *
 * @return string|null -> normalized URL, or null when unsupported
 */
function link_previews__url(value)
{
    try
    {
        let url = new URL(value);
        if ((url.protocol !== "https:" && url.protocol !== "http:") || url.username || url.password)
        {
            return null;
        }
        url.hash = "";
        return url.href.length <= 2048 ? url.href : null;
    }
    catch (error)
    {
        return null;
    }
}

/**
 * @brief extracts up to two distinct URLs while trimming surrounding punctuation
 *
 * @param string text -> message text to scan
 *
 * @return array -> normalized URLs in message order
 */
function link_previews__extract(text)
{
    let urls = [];
    let matches = text.match(/https?:\/\/[^\s<>"']+/gi) || [];
    for (let match of matches)
    {
        match = match.replace(/[.,;!?]+$/, "");
        for (let pair of ["()", "[]", "{}"])
        {
            while (match.endsWith(pair[1]) && match.split(pair[1]).length > match.split(pair[0]).length)
            {
                match = match.slice(0, -1);
            }
        }
        let url = link_previews__url(match);
        if (url !== null && !urls.includes(url))
        {
            urls.push(url);
        }
        if (urls.length === 2)
        {
            break;
        }
    }
    return urls;
}

// render page text as text and use only image bytes returned by the server
// a preview must not make each reader contact the website directly
/**
 * @brief replaces a message's preview cards using successful cached server results
 *
 * @param HTMLElement paragraph -> message text element
 * @param object state -> message URLs and current preview card container
 *
 * @return void
 */
function link_previews__render(paragraph, state)
{
    if (state.cards !== null)
    {
        state.cards.remove();
        state.cards = null;
    }
    if (!paragraph.isConnected || !g_server_policy.is_link_preview_allowed)
    {
        return;
    }
    let cards = document.createElement("div");
    cards.className = "chat-link-previews";
    for (let url of state.urls)
    {
        let entry = g_link_previews.urls.get(url);
        let result = entry && entry.result;
        if (!result || result.status !== "ok")
        {
            continue;
        }
        let card = document.createElement("a");
        card.className = "chat-link-preview";
        card.href = url;
        card.target = "_blank";
        card.rel = "noopener noreferrer";
        for (let field of ["site_name", "title", "description"])
        {
            let value = typeof result[field] === "string" ? result[field].slice(0, 1024) : "";
            if (field === "site_name" && !value)
            {
                value = new URL(url).hostname;
            }
            if (value)
            {
                let line = document.createElement("span");
                line.className = "chat-link-preview-" + field;
                line.textContent = value;
                card.appendChild(line);
            }
        }
        if (typeof result.image === "string" && result.image.length <= 350000 && /^data:image\/(png|jpeg|gif|webp);base64,[A-Za-z0-9+/]+={0,2}$/.test(result.image))
        {
            let image = document.createElement("img");
            image.src = result.image;
            image.alt = "";
            image.loading = "lazy";
            image.addEventListener("load", function()
            {
                chat__scroll_chat_to_end(false);
            });
            image.addEventListener("error", function()
            {
                image.remove();
            });
            card.appendChild(image);
        }
        cards.appendChild(card);
    }
    if (cards.childElementCount > 0)
    {
        paragraph.insertAdjacentElement("afterend", cards);
        state.cards = cards;
        chat__scroll_chat_to_end(false);
    }
}

/**
 * @brief removes a message's preview cards and tracked state
 *
 * @param HTMLElement paragraph -> message text element
 *
 * @return void
 */
function link_previews__forget(paragraph)
{
    let state = g_link_previews.messages.get(paragraph);
    if (state)
    {
        if (state.cards)
        {
            state.cards.remove();
        }
        g_link_previews.messages.delete(paragraph);
    }
}

/**
 * @brief updates a changed message's URLs and queues uncached previews within the cache limit
 *
 * @param HTMLElement paragraph -> message text element
 *
 * @return void
 */
function link_previews__scan(paragraph)
{
    let text = paragraph.textContent;
    let previous = g_link_previews.messages.get(paragraph);
    if (previous && previous.text === text)
    {
        return;
    }
    link_previews__forget(paragraph);
    let urls = link_previews__extract(text);
    if (urls.length === 0)
    {
        return;
    }
    let state = { text: text, urls: urls, cards: null };
    g_link_previews.messages.set(paragraph, state);
    for (let url of urls)
    {
        if (!g_link_previews.urls.has(url))
        {
            // bound the stored images and requests even in a very long chat
            if (g_link_previews.urls.size >= 64)
            {
                let removed = false;
                for (let [old_url, entry] of g_link_previews.urls)
                {
                    if (entry.finished)
                    {
                        g_link_previews.urls.delete(old_url);
                        removed = true;
                        break;
                    }
                }
                if (!removed)
                {
                    continue;
                }
            }
            g_link_previews.urls.set(url, { request_id: null, sent: 0, finished: false, result: null });
        }
    }
    link_previews__render(paragraph, state);
}

// send a few requests at a time. a missing reply must not keep the queue stuck
// forever, for example when the connection drops during a website lookup
/**
 * @brief expires stalled requests and sends one needed preview while fewer than four are pending
 *
 * @return void
 */
function link_previews__pump()
{
    if (!g_is_authenticated || !g_server_policy.is_link_preview_allowed)
    {
        return;
    }
    let in_flight = 0;
    for (let entry of g_link_previews.urls.values())
    {
        if (entry.sent && !entry.finished)
        {
            if (Date.now() - entry.sent > 90000)
            {
                entry.finished = true;
            }
            else
            {
                in_flight++;
            }
        }
    }
    if (in_flight >= 4)
    {
        return;
    }
    for (let [url, entry] of g_link_previews.urls)
    {
        if (!entry.sent && !entry.finished)
        {
            let needed = false;
            for (let [paragraph, state] of g_link_previews.messages)
            {
                if (paragraph.isConnected && state.urls.includes(url))
                {
                    needed = true;
                    break;
                }
            }
            if (!needed)
            {
                g_link_previews.urls.delete(url);
                continue;
            }
            entry.request_id = "preview-" + (++g_link_previews.sequence);
            entry.sent = Date.now();
            connection__send_message_object({ message: { type: "resolve_url", request_id: entry.request_id, url: url } });
            break;
        }
    }
}

/**
 * @brief accepts a matching preview response and refreshes messages using its URL
 *
 * @param object message -> server preview result with URL and request ID
 *
 * @return void
 */
function link_previews__resolved(message)
{
    if (!message || !g_server_policy.is_link_preview_allowed)
    {
        return;
    }
    let entry = g_link_previews.urls.get(message.url);
    if (!entry || entry.finished || !entry.request_id || entry.request_id !== message.request_id)
    {
        return;
    }
    entry.finished = true;
    entry.result = message;
    for (let [paragraph, state] of g_link_previews.messages)
    {
        if (state.urls.includes(message.url))
        {
            link_previews__render(paragraph, state);
        }
    }
}

/**
 * @brief stops preview observation and clears pending requests and displayed cards
 *
 * @return void
 */
function link_previews__reset()
{
    if (g_link_previews.observer)
    {
        g_link_previews.observer.disconnect();
        g_link_previews.observer = null;
    }
    clearInterval(g_link_previews.timer);
    g_link_previews.timer = null;
    for (let paragraph of g_link_previews.messages.keys())
    {
        link_previews__forget(paragraph);
    }
    g_link_previews.urls.clear();
}

/**
 * @brief starts or stops message observation and preview requests according to server policy
 *
 * @return void
 */
function link_previews__apply_policy()
{
    if (!g_server_policy.is_link_preview_allowed)
    {
        link_previews__reset();
        return;
    }
    if (typeof MutationObserver === "undefined" || g_link_previews.observer)
    {
        return;
    }
    let container = document.getElementById("chat-context-container");
    if (!container)
    {
        return;
    }
    let selector = ".single-chat-message-content-p";
    g_link_previews.observer = new MutationObserver(function(changes)
    {
        for (let paragraph of g_link_previews.messages.keys())
        {
            if (!container.contains(paragraph))
            {
                link_previews__forget(paragraph);
            }
        }
        let paragraphs = new Set();
        for (let change of changes)
        {
            let target = change.target.nodeType === 1 ? change.target : change.target.parentElement;
            let paragraph = target && target.closest(selector);
            if (paragraph)
            {
                paragraphs.add(paragraph);
            }
            for (let node of change.addedNodes)
            {
                if (node.nodeType === 1 && !node.closest(".chat-link-previews"))
                {
                    if (node.matches(selector))
                    {
                        paragraphs.add(node);
                    }
                    for (let added of node.querySelectorAll(selector))
                    {
                        paragraphs.add(added);
                    }
                }
            }
        }
        for (let paragraph of paragraphs)
        {
            if (container.contains(paragraph))
            {
                link_previews__scan(paragraph);
            }
        }
    });
    g_link_previews.observer.observe(container, { childList: true, subtree: true, characterData: true });
    for (let paragraph of container.querySelectorAll(selector))
    {
        link_previews__scan(paragraph);
    }
    g_link_previews.timer = setInterval(link_previews__pump, 750);
}
