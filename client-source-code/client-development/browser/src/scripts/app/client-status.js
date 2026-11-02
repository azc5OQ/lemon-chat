var client_status =
{
    profile: null,
    popup: null,
    pending: new Map(),
    samples: new WeakMap(),
    next_request: 0,
    timer: null
};

/**
 * @brief Formats a connection duration using the two largest useful units.
 * @param seconds -> elapsed seconds reported by the server
 */
function client_status__format_duration(seconds)
{
    let remaining = Math.max(0, Math.floor(seconds));
    let parts = [];
    let units = [[2592000, "month"], [86400, "day"], [3600, "hour"], [60, "minute"], [1, "second"]];
    for (let [length, name] of units)
    {
        let amount = Math.floor(remaining / length);
        if (amount > 0 || length === 1 && parts.length === 0)
        {
            parts.push(amount + " " + name + (amount === 1 ? "" : "s"));
            remaining %= length;
        }
        if (parts.length === 2)
        {
            break;
        }
    }
    return parts.join(" ");
}

/**
 * @brief Keeps the profile's connection time compact, without seconds.
 * @param seconds Elapsed seconds reported by the server.
 */
function client_status__format_profile_duration(seconds)
{
    if (!Number.isFinite(seconds))
    {
        return "Unavailable";
    }
    let total_minutes = Math.max(0, Math.floor(seconds / 60));
    if (total_minutes < 60)
    {
        return total_minutes + " minute" + (total_minutes === 1 ? "" : "s");
    }
    let days = Math.floor(total_minutes / 1440);
    let hours = Math.floor(total_minutes / 60) % 24;
    let minutes = total_minutes % 60;
    let prefix = days > 0 ? days + " day" + (days === 1 ? "" : "s") + " " : "";
    return prefix + hours + ":" + String(minutes).padStart(2, "0")
        + (hours === 1 && minutes === 0 ? " hour" : " hours");
}

/**
 * @brief Requests details for the selected client and remembers which view requested them.
 * @param view -> profile or popup
 * @param client -> selected connected client
 */
function client_status__select(view, client)
{
    client_status[view] = client;
    client_status.pending.delete(view);
    let allowed = groups__has_permission("view_client_connection_time")
        || view === "popup" && groups__has_permission("view_client_details");
    if (client && allowed)
    {
        let request = ++client_status.next_request;
        client_status.pending.set(view,
        {
            request, client, at: performance.now()
        });
        connection__send_message_object(
        {
            message:
            {
                type: "get_client_info", client_id: Number(client.client_id), request_id: request
            }
        });
    }
    if (client && !client_status.timer)
    {
        client_status.timer = setInterval(client_status__render, 1000);
    }
    client_status__render();
}

/**
 * @brief Applies a reply only to the client and view that requested it.
 * @param message -> client details supplied by the server
 */
function client_status__receive(message)
{
    for (let [view, pending] of client_status.pending)
    {
        if (pending.request !== message.request_id || Number(pending.client.client_id) !== message.client_id)
        {
            continue;
        }
        client_status.pending.delete(view);
        if (!g_is_authenticated || channel_tree__get_client_by_client_id(message.client_id) !== pending.client)
        {
            continue;
        }
        if (groups__has_permission("view_client_connection_time")
            && Number.isFinite(message.connected_seconds) && message.connected_seconds >= 0)
        {
            client_status.samples.set(pending.client,
            {
                seconds: message.connected_seconds, at: performance.now()
            });
        }
        if (view === "popup" && groups__has_permission("view_client_details"))
        {
            document.getElementById("client-info-ip").textContent = message.ip_address || "Not permitted";
            document.getElementById("client-info-country").textContent = message.country_iso_code || "Unknown";
            document.getElementById("client-info-last-action").textContent = Number.isFinite(message.last_action_seconds_ago)
                ? client_status__format_duration(message.last_action_seconds_ago) + " ago" : "Unavailable";
            document.getElementById("client-info-identity").textContent = message.identity || "Unavailable";
        }
    }
    client_status__render();
}

/**
 * @brief Updates visible duration values locally without requesting the server every second.
 */
function client_status__render()
{
    if (!G_HAS_DOM)
    {
        return;
    }
    let allowed = groups__has_permission("view_client_connection_time");
    for (let [view, element_id] of [["profile", "current-client-connected"], ["popup", "client-info-connected"]])
    {
        let element = document.getElementById(element_id);
        let client = client_status[view];
        element.parentElement.hidden = !allowed || !client;
        if (!allowed)
        {
            element.textContent = "";
        }
        if (!client || !element.getClientRects().length)
        {
            continue;
        }
        if (!g_is_authenticated || channel_tree__get_client_by_client_id(client.client_id) !== client)
        {
            element.textContent = "Disconnected";
            continue;
        }
        let sample = client_status.samples.get(client);
        let pending = client_status.pending.get(view);
        let format_duration = view === "profile" ? client_status__format_profile_duration : client_status__format_duration;
        element.textContent = sample
            ? format_duration(sample.seconds + (performance.now() - sample.at) / 1000)
            : pending && performance.now() - pending.at < 10000 ? "Loading…" : "Unavailable";
    }
    for (let field of ["ip", "country", "last-action", "identity"])
    {
        let element = document.getElementById("client-info-" + field);
        element.parentElement.hidden = !groups__has_permission("view_client_details");
    }
}

/**
 * @brief Discards old replies and requests visible details again after permissions change.
 */
function client_status__permissions_changed()
{
    client_status.samples = new WeakMap();
    client_status.pending.clear();
    for (let field of ["ip", "country", "last-action", "identity"])
    {
        document.getElementById("client-info-" + field).textContent = "";
    }
    for (let [view, id] of [["profile", "current-client-description"], ["popup", "client-info-container"]])
    {
        if (client_status[view] && document.getElementById(id).getClientRects().length)
        {
            client_status__select(view, client_status[view]);
        }
    }
    client_status__render();
}

/**
 * @brief Clears connection details when leaving a server.
 */
function client_status__reset()
{
    clearInterval(client_status.timer);
    client_status.timer = null;
    client_status.profile = null;
    client_status.popup = null;
    client_status.pending.clear();
    client_status.samples = new WeakMap();
    for (let id of ["current-client-connected", "client-info-connected"])
    {
        document.getElementById(id).textContent = "";
    }
    client_status__render();
}
