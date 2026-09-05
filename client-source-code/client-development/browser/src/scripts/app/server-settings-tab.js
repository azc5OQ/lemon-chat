// server-settings-tab.js is embedded in template.html along with the other client files
// it is the admin's server settings tab: the field table that loads and saves the general and log
// settings, the policy the server announces, the icon upload queue and the country block list
// dispatch.js feeds it the server's values, ui.js has the click handlers

// the server settings tab, one row per input: "key" is the wire name, "kind" says checkbox or
// number field, "tab" which save button sends it. the load and both saves walk this table
var SERVER_SETTINGS_FIELDS = [
    { key: "display_country_flags", id: "server-settings-general-display-flags-checkbox", kind: "bool", tab: "general" },
    { key: "hide_admin_country_flag", id: "server-settings-general-hide-admin-flag-checkbox", kind: "bool", tab: "general" },
    { key: "enable_audio", id: "server-settings-general-enable-audio", kind: "bool", tab: "general" },
    { key: "enable_music_bot_audio", id: "server-settings-general-enable-music-bot-audio-checkbox", kind: "bool", tab: "general" },
    { key: "hide_clients_in_password_channels", id: "server-settings-general-hide-clients-in-password-protected-channels", kind: "bool", tab: "general" },
    { key: "allow_temp_channels", id: "server-settings-general-allow-temp-channels-checkbox", kind: "bool", tab: "general" },
    { key: "allow_typing_indicator", id: "server-settings-general-allow-typing-indicator-checkbox", kind: "bool", tab: "general" },
    { key: "allow_client_renames", id: "server-settings-general-allow-renames-checkbox", kind: "bool", tab: "general" },
    { key: "is_sending_text_to_idle_clients_allowed", id: "server-settings-general-allow-text-to-idle-clients-checkbox", kind: "bool", tab: "general" },
    { key: "allow_private_messages", id: "server-settings-general-allow-private-messages-checkbox", kind: "bool", tab: "general" },
    { key: "allow_file_uploads", id: "server-settings-general-allow-file-uploads-checkbox", kind: "bool", tab: "general" },
    { key: "file_upload_max_size_mb", id: "server-settings-general-file-upload-max-size-input", kind: "number", fallback: 10, tab: "general" },
    { key: "allow_chat_pictures", id: "server-settings-general-allow-pictures-checkbox", kind: "bool", tab: "general" },
    { key: "chat_picture_max_size_mb", id: "server-settings-general-picture-max-size-input", kind: "number", fallback: 4, tab: "general" },
    { key: "is_same_ip_address_allowed", id: "server-settings-general-allow-same-ip-checkbox", kind: "bool", tab: "general" },
    { key: "is_fast_reconnect_allowed", id: "server-settings-general-fast-reconnect-checkbox", kind: "bool", tab: "general" },
    { key: "is_identity_takeover_allowed", id: "server-settings-general-identity-takeover-checkbox", kind: "bool", tab: "general" },
    { key: "is_websocket_ping_active", id: "server-settings-general-websocket-ping-checkbox", kind: "bool", tab: "general" },
    { key: "webrtc_datachannel_cooldown_seconds", id: "server-settings-general-datachannel-cooldown-input", kind: "number", fallback: 600, tab: "general" },
    { key: "icon_max_size_bytes", id: "server-settings-general-icon-max-size-input", kind: "number", fallback: 5000, tab: "general" },
    { key: "show_music_bot_marquee_to_everyone", id: "server-settings-general-marquee-everyone-checkbox", kind: "bool", tab: "general" },
    { key: "minimum_rsa_key_bits", id: "server-settings-general-minimum-rsa-bits-input", kind: "number", fallback: 2048, tab: "general" },
    { key: "announce_minimum_rsa_key_bits", id: "server-settings-general-announce-rsa-bits-checkbox", kind: "bool", tab: "general" },
    { key: "is_country_blocking_active", id: "server-settings-general-country-blocking-checkbox", kind: "bool", tab: "general" },
    { key: "log_client_joins", id: "server-settings-log-joins-checkbox", kind: "bool", tab: "log" },
    { key: "log_username_changes", id: "server-settings-log-renames-checkbox", kind: "bool", tab: "log" },
    { key: "log_tag_changes", id: "server-settings-log-tags-checkbox", kind: "bool", tab: "log" },
    { key: "log_server_settings_updates", id: "server-settings-log-settings-checkbox", kind: "bool", tab: "log" },
    { key: "log_kicks_and_bans", id: "server-settings-log-kicks-bans-checkbox", kind: "bool", tab: "log" },
    { key: "log_client_disconnects", id: "server-settings-log-disconnects-checkbox", kind: "bool", tab: "log" },
    { key: "log_failed_attempts", id: "server-settings-log-failed-checkbox", kind: "bool", tab: "log" },
    { key: "admin_log_max_size_mb", id: "server-settings-log-max-size-input", kind: "number", fallback: 10, tab: "log" },
    { key: "admin_log_retention_days", id: "server-settings-log-retention-select", kind: "number", fallback: 7, tab: "log" }
];

// stylesheet harvest and the Intl helper, both resolved once on first use
var country_code_cache = null;

var country_display_names = null;

var is_country_select_populated = false;

/**
 * @brief fills the tab's inputs from a server_settings_values message; a number the server left out shows its fallback
 *
 * @param object values -> the message body
 *
 * @return void
 */
function server_settings_tab__apply_server_settings_values_to_tab(values)
{
    for (let i = 0; i < SERVER_SETTINGS_FIELDS.length; i++)
    {
        let field = SERVER_SETTINGS_FIELDS[i];
        let element = document.getElementById(field.id);

        if (field.kind == "bool")
        {
            element.checked = (values[field.key] == true);
        }
        else
        {
            element.value = (typeof values[field.key] === "number") ? values[field.key] : field.fallback;
        }
    }
}

/**
 * @brief reads one tab's inputs back into a save_server_settings message body
 *        an empty number field sends its fallback; 0 stays 0, because some fields mean "off" by it
 *
 * @param string tab -> "general" or "log", which fields to read
 *
 * @return object the message body, type included
 */
function server_settings_tab__collect_server_settings_from_tab(tab)
{
    let settings = { type: "save_server_settings" };

    for (let i = 0; i < SERVER_SETTINGS_FIELDS.length; i++)
    {
        let field = SERVER_SETTINGS_FIELDS[i];

        if (field.tab != tab)
        {
            continue;
        }

        let element = document.getElementById(field.id);

        if (field.kind == "bool")
        {
            settings[field.key] = element.checked;
        }
        else
        {
            let number = parseInt(element.value);
            settings[field.key] = isNaN(number) ? field.fallback : number;
        }
    }

    return settings;
}

/**
 * @brief applies the server's client-facing policy to g_server_policy, from authentication_status on join and from the "server_policy" broadcast an admin's settings save triggers
 *        a field an older server does not send keeps its current value; a number is taken only when positive
 *
 * @param object data -> the message body
 *
 * @return void
 */
function server_settings_tab__apply_server_policy_fields(data)
{
    for (let policy_field in g_server_policy)
    {
        if (data[policy_field] === undefined)
        {
            continue;
        }

        if (typeof g_server_policy[policy_field] === "number")
        {
            if (typeof data[policy_field] === "number" && data[policy_field] > 0)
            {
                g_server_policy[policy_field] = data[policy_field];
            }
        }
        else
        {
            g_server_policy[policy_field] = (data[policy_field] == true);
        }
    }

    server_settings_tab__apply_rename_policy_to_ui();
    chat_files__apply_file_upload_policy_to_ui();
    chat_files__apply_chat_picture_policy_to_ui();
}

/**
 * @brief greys the local rename input when the server ignores user renames, because a rename that silently does nothing reads as a bug; an admin keeps the input editable
 *
 * @return void
 */
function server_settings_tab__apply_rename_policy_to_ui()
{
    let rename_input = document.getElementById("connected-local-client-input");

    if (rename_input == null)
    {
        return;
    }

    let may_rename = (g_server_policy.allow_client_renames == true) || (g_is_local_client_admin == true);

    rename_input.readOnly = (may_rename == false);
    rename_input.title = (may_rename == false) ? "renames are disabled on this server" : "";
}

/**
 * @brief the icon upload pump: sends the next queued icon unless one is in flight
 *        the reply handler clears g_icon_upload_in_flight_base64 and calls this again for the rest of the queue
 *
 * @return void
 */
function server_settings_tab__send_next_queued_icon_upload()
{
    if (g_icon_upload_in_flight_base64 != null) // still waiting for the previous upload's reply
    {
        return;
    }
    if (g_icon_upload_queue.length == 0)
    {
        return;
    }

    let base64_icon = g_icon_upload_queue.shift();
    g_icon_upload_in_flight_base64 = base64_icon;

    let message_object = {
        message: {
            type: "server_settings_icon_upload",
            base64_icon_value: base64_icon
        }
    };

    connection__send_message_object(message_object);
}

/**
 * @brief english name for an iso country code
 *
 * @param string code -> the two-letter code
 *
 * @return string the name, or the code itself when the browser cannot name it
 */
function server_settings_tab__get_country_display_name(code)
{
    if (country_display_names === null)
    {
        try
        {
            country_display_names = new Intl.DisplayNames(["en"], { type: "region" });
        }
        catch (intl_error)
        {
            country_display_names = false;
        }
    }

    if (country_display_names !== false)
    {
        try
        {
            let name = country_display_names.of(code);

            if (typeof name === "string" && name.length > 0)
            {
                return name;
            }
        }
        catch (lookup_error) { }
    }

    return code;
}

/**
 * @brief every code the flag stylesheet can draw, sorted by display name
 *
 * @return array the codes, [] headless
 */
function server_settings_tab__get_all_country_codes()
{
    if (country_code_cache != null)
    {
        return country_code_cache;
    }

    let codes = {};

    if (typeof document !== "undefined" && document.styleSheets != null)
    {
        for (let sheet_index = 0; sheet_index < document.styleSheets.length; sheet_index++)
        {
            let rules = null;

            try
            {
                rules = document.styleSheets[sheet_index].cssRules;
            }
            catch (rules_error)
            {
                continue;
            }

            if (rules == null)
            {
                continue;
            }

            for (let rule_index = 0; rule_index < rules.length; rule_index++)
            {
                let selector = rules[rule_index].selectorText;

                if (typeof selector !== "string")
                {
                    continue;
                }

                let matches = selector.match(/\.country-flag-([a-z]{2})\b/g);

                if (matches == null)
                {
                    continue;
                }

                for (let match_index = 0; match_index < matches.length; match_index++)
                {
                    codes[matches[match_index].slice(-2).toUpperCase()] = true;
                }
            }
        }
    }

    country_code_cache = Object.keys(codes).sort(function(a, b)
    {
        return server_settings_tab__get_country_display_name(a).localeCompare(server_settings_tab__get_country_display_name(b));
    });

    return country_code_cache;
}

/**
 * @brief fills the country picker once, the first time the section becomes visible
 *
 * @return void
 */
function server_settings_tab__populate_country_block_select()
{
    if (is_country_select_populated == true)
    {
        return;
    }

    let select = document.getElementById("server-settings-country-block-select");

    if (select == null)
    {
        return;
    }

    let codes = server_settings_tab__get_all_country_codes();

    if (codes.length == 0)
    {
        return;
    }

    let html = "<option value=\"\">add a country to the block list ...</option>";

    for (let i = 0; i < codes.length; i++)
    {
        html += "<option value=\"" + codes[i] + "\">" + chat__sanitize_string(server_settings_tab__get_country_display_name(codes[i])) + " (" + codes[i] + ")</option>";
    }

    select.innerHTML = html;
    is_country_select_populated = true;
}

/**
 * @brief repaints the blocked-countries list from g_blocked_countries_draft, "no blocked countries" when empty
 *
 * @return void
 */
function server_settings_tab__render_blocked_countries_list()
{
    let container = document.getElementById("server-settings-blocked-countries-list");

    if (container == null)
    {
        return;
    }

    container.innerHTML = "";

    if (g_blocked_countries_draft.length == 0)
    {
        let empty = document.createElement("p");
        empty.className = "blocked-country-empty";
        empty.innerText = "no blocked countries";
        container.appendChild(empty);
        return;
    }

    for (let i = 0; i < g_blocked_countries_draft.length; i++)
    {
        let code = g_blocked_countries_draft[i];

        let row = document.createElement("div");
        row.className = "blocked-country-entry";

        let flag = document.createElement("div");
        flag.className = "blocked-country-flag country-flag-" + code.toLowerCase();
        row.appendChild(flag);

        let name = document.createElement("span");
        name.className = "blocked-country-name";
        name.innerText = server_settings_tab__get_country_display_name(code) + " (" + code + ")";
        row.appendChild(name);

        let remove = document.createElement("span");
        remove.className = "blocked-country-remove";
        remove.title = "remove";
        remove.innerText = "✕";
        remove.setAttribute("data-country-code", code);
        remove.onclick = function(event)
        {
            event.stopPropagation();

            let removed_code = event.currentTarget.getAttribute("data-country-code");

            g_blocked_countries_draft = g_blocked_countries_draft.filter(function(entry) { return entry != removed_code; });
            server_settings_tab__render_blocked_countries_list();
        };
        row.appendChild(remove);

        container.appendChild(row);
    }
}

/**
 * @brief picking an option adds it to the draft (at most 100, sorted by name); the picker snaps back to its placeholder
 *
 * @return void
 */
function server_settings_tab__country_block_select_onchange()
{
    let select = document.getElementById("server-settings-country-block-select");
    let code = select.value;

    select.value = "";

    if (typeof code !== "string" || /^[A-Z]{2}$/.test(code) == false)
    {
        return;
    }

    if (g_blocked_countries_draft.indexOf(code) != -1)
    {
        return;
    }

    if (g_blocked_countries_draft.length >= 100)
    {
        utils__custom_alert("the block list is full (100 countries)");
        return;
    }

    g_blocked_countries_draft.push(code);
    g_blocked_countries_draft.sort(function(a, b)
    {
        return server_settings_tab__get_country_display_name(a).localeCompare(server_settings_tab__get_country_display_name(b));
    });
    server_settings_tab__render_blocked_countries_list();
}

/**
 * @brief the "hide admin's flag" row only makes sense while flags are displayed at all, so it follows the flags checkbox
 *
 * @return void
 */
function server_settings_tab__refresh_hide_admin_flag_row_visibility()
{
    let flags_checkbox = document.getElementById("server-settings-general-display-flags-checkbox");
    let row = document.getElementById("server-settings-hide-admin-flag-row");

    if (flags_checkbox == null || row == null)
    {
        return;
    }

    row.style.display = (flags_checkbox.checked == true) ? "" : "none";
}

/**
 * @brief the country-blocking checkbox shows or hides the picker below it
 *        the block list itself is server state either way; unchecking just disables enforcement, it does not clear the list
 *
 * @return void
 */
function server_settings_tab__refresh_country_blocking_visibility()
{
    let checkbox = document.getElementById("server-settings-general-country-blocking-checkbox");
    let container = document.getElementById("server-settings-country-blocking-container");

    if (checkbox == null || container == null)
    {
        return;
    }

    if (checkbox.checked == true)
    {
        server_settings_tab__populate_country_block_select();
        container.style.display = "block";
    }
    else
    {
        container.style.display = "none";
    }
}

/**
 * @brief takes the blocked countries the server currently has, straight from server_settings_values, into the draft
 *
 * @param array list -> the two-letter codes
 *
 * @return void
 */
function server_settings_tab__set_blocked_countries_from_server(list)
{
    g_blocked_countries_draft = [];

    if (Array.isArray(list))
    {
        for (let i = 0; i < list.length; i++)
        {
            if (typeof list[i] === "string" && /^[A-Za-z]{2}$/.test(list[i]) == true
                && g_blocked_countries_draft.indexOf(list[i].toUpperCase()) == -1)
            {
                g_blocked_countries_draft.push(list[i].toUpperCase());
            }
        }
    }

    server_settings_tab__render_blocked_countries_list();
}
