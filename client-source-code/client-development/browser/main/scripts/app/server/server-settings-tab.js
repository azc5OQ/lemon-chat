// server-settings-tab.js is embedded in template.html along with the other client files
// it is the admin's server settings tab: the field table that loads and saves the general and log
// settings, the policy the server announces, the icon upload queue and the country block list
// dispatch.js feeds it the server's values, ui.js has the click handlers

// the server settings tab, one row per input: "key" is the field name in the settings message, "kind" says checkbox or
// number field, "tab" which save button sends it. the load and both saves walk this table
var SERVER_SETTINGS_FIELDS = [
    { key: "is_client_http_enabled", id: "server-http-enabled", kind: "bool", tab: "general" },
    { key: "is_client_http_blank_when_disabled", id: "server-http-disable-mode", kind: "http-mode", tab: "general" },
    { key: "is_client_config_embedded", id: "server-http-embed", kind: "bool", tab: "general" },

    // start page buttons and default themes of the served page
    { key: "is_start_page_connect_button_enabled", id: "server-start-page-connect-button", kind: "bool", tab: "general" },
    { key: "is_start_page_import_identity_button_enabled", id: "server-start-page-import-identity-button", kind: "bool", tab: "general" },
    { key: "default_theme", id: "server-default-theme", kind: "select", fallback: "", tab: "general" },
    { key: "default_mobile_theme", id: "server-default-mobile-theme", kind: "select", fallback: "", tab: "general" },
    {
        key: "dh_exchange_count",
        id: "server-encryption-dh_exchange_count",
        kind: "number",
        fallback: 1,
        min: 1, max: 4,
        tab: "general"
    },
    {
        key: "session_encryption_layer_count",
        id: "server-encryption-session_encryption_layer_count",
        kind: "number",
        fallback: 1,
        min: 1, max: 8,
        tab: "general"
    },
    {
        key: "session_cipher",
        id: "server-encryption-session_cipher",
        kind: "select",
        fallback: "aes256",

        tab: "general"
    },
    {
        key: "is_random_cipher_layer_enabled",
        id: "server-encryption-is_random_cipher_layer_enabled",
        kind: "bool",
        fallback: false,

        tab: "general"
    },
    {
        key: "max_channel_count",
        id: "server-settings-general-max-channel-count",
        kind: "number",
        fallback: 100,
        min: 1,
        max: 2147483647,
        tab: "general"
    },
    {
        key: "is_blocking_direct_chat_pictures_active",
        id: "server-settings-allow-block-direct-chat-pictures",
        kind: "bool",
        tab: "general"
    },
    {
        key: "is_blocking_direct_chat_files_active",
        id: "server-settings-allow-block-direct-chat-files",
        kind: "bool",
        tab: "general"
    },
    {
        key: "is_blocking_channel_chat_pictures_active",
        id: "server-settings-allow-block-channel-chat-pictures",
        kind: "bool",
        tab: "general"
    },
    {
        key: "is_blocking_channel_chat_files_active",
        id: "server-settings-allow-block-channel-chat-files",
        kind: "bool",
        tab: "general"
    },
    { key: "is_server_uptime_published", id: "server-settings-publish-uptime", kind: "bool", fallback: true, tab: "general" },
    { key: "reserved_slots", id: "server-settings-reserved-slots", kind: "number", fallback: 0, tab: "general" },
    { key: "is_server_slots_published", id: "server-settings-publish-slots", kind: "bool", fallback: true, tab: "general" },
    { key: "is_server_banner_published", id: "server-settings-publish-banner", kind: "bool", fallback: true, tab: "general" },
    { key: "is_server_features_published", id: "server-settings-publish-features", kind: "bool", fallback: true, tab: "general" },
    { key: "is_display_country_flags_active", id: "server-settings-general-display-flags-checkbox", kind: "bool", tab: "general" },
    { key: "is_admin_country_flag_hidden", id: "server-settings-general-hide-admin-flag-checkbox", kind: "bool", tab: "general" },
    { key: "is_voice_chat_active", id: "server-settings-general-enable-audio", kind: "bool", tab: "general" },
    { key: "audio_connections", id: "server-settings-general-audio-connections", kind: "select", fallback: "both", tab: "general" },
    { key: "is_music_bot_audio_active", id: "server-settings-general-enable-music-bot-audio-checkbox", kind: "bool", tab: "general" },
    { key: "is_video_streaming_active", id: "server-settings-general-enable-video-streaming-checkbox", kind: "bool", tab: "general" },
    { key: "is_hide_clients_in_password_protected_channels_active", id: "server-settings-general-hide-clients-in-password-protected-channels", kind: "bool", tab: "general" },
    { key: "is_temp_channel_creation_allowed", id: "server-settings-general-allow-temp-channels-checkbox", kind: "bool", tab: "general" },
    { key: "is_typing_indicator_allowed", id: "server-settings-general-allow-typing-indicator-checkbox", kind: "bool", tab: "general" },
    { key: "is_client_rename_allowed", id: "server-settings-general-allow-renames-checkbox", kind: "bool", tab: "general" },
    { key: "is_link_preview_allowed", id: "server-settings-general-link-previews-checkbox", kind: "bool", tab: "general" },
    { key: "is_sending_text_to_idle_clients_allowed", id: "server-settings-general-allow-text-to-idle-clients-checkbox", kind: "bool", tab: "general" },
    { key: "is_private_messaging_allowed", id: "server-settings-general-allow-private-messages-checkbox", kind: "bool", tab: "general" },
    { key: "is_file_upload_allowed", id: "server-settings-general-allow-file-uploads-checkbox", kind: "bool", tab: "general" },
    { key: "file_upload_max_size_mb", id: "server-settings-general-file-upload-max-size-input", kind: "number", fallback: 10, tab: "general" },
    { key: "is_chat_picture_allowed", id: "server-settings-general-allow-pictures-checkbox", kind: "bool", tab: "general" },
    { key: "chat_picture_max_size_mb", id: "server-settings-general-picture-max-size-input", kind: "number", fallback: 4, tab: "general" },
    { key: "is_same_ip_address_allowed", id: "server-settings-general-allow-same-ip-checkbox", kind: "bool", tab: "general" },
    { key: "is_fast_reconnect_allowed", id: "server-settings-general-fast-reconnect-checkbox", kind: "bool", tab: "general" },
    { key: "is_identity_takeover_allowed", id: "server-settings-general-identity-takeover-checkbox", kind: "bool", tab: "general" },
    { key: "is_websocket_ping_active", id: "server-settings-general-websocket-ping-checkbox", kind: "bool", tab: "general" },
    { key: "is_connection_check_enforced", id: "server-settings-general-connection-check-checkbox", kind: "bool", fallback: true, tab: "general" },
    { key: "is_channel_key_reuse_enabled", id: "server-settings-channel-key-reuse-enabled", kind: "bool", fallback: true, tab: "general" },
    { key: "is_channel_key_reuse_on_join_allowed", id: "server-settings-channel-key-reuse-on-join", kind: "bool", fallback: true, tab: "general" },
    { key: "is_channel_key_reuse_on_leave_allowed", id: "server-settings-channel-key-reuse-on-leave", kind: "bool", fallback: true, tab: "general" },
    { key: "channel_key_reuse_join_min_clients", id: "server-settings-channel-key-reuse-join-min-clients", kind: "number", fallback: 50, min: 30, max: 1000000, tab: "general" },
    { key: "channel_key_reuse_leave_min_clients", id: "server-settings-channel-key-reuse-leave-min-clients", kind: "number", fallback: 100, min: 30, max: 1000000, tab: "general" },
    { key: "max_concurrent_logins", id: "server-settings-general-concurrent-logins-input", kind: "number", fallback: 4, tab: "general" },
    { key: "webrtc_datachannel_cooldown_seconds", id: "server-settings-general-datachannel-cooldown-input", kind: "number", fallback: 600, tab: "general" },
    { key: "icon_max_size_bytes", id: "server-settings-general-icon-max-size-input", kind: "number", fallback: 5000, tab: "general" },
    { key: "avatar_max_size_kb", id: "server-settings-general-avatar-max-size-input", kind: "number", fallback: 50, min: 1, max: 90, tab: "general" },
    { key: "is_music_bot_marquee_visible_to_everyone", id: "server-settings-general-marquee-everyone-checkbox", kind: "bool", tab: "general" },
    { key: "minimum_rsa_key_bits", id: "server-settings-general-minimum-rsa-bits-input", kind: "number", fallback: 2048, tab: "general" },
    { key: "is_minimum_rsa_key_bits_announced", id: "server-settings-general-announce-rsa-bits-checkbox", kind: "bool", tab: "general" },
    { key: "is_country_blocking_active", id: "server-settings-general-country-blocking-checkbox", kind: "bool", tab: "general" },
    { key: "is_logging_client_joins_enabled", id: "server-settings-log-joins-checkbox", kind: "bool", tab: "log" },
    { key: "is_logging_username_changes_enabled", id: "server-settings-log-renames-checkbox", kind: "bool", tab: "log" },
    { key: "is_logging_group_changes_enabled", id: "server-settings-log-groups-checkbox", kind: "bool", tab: "log" },
    { key: "is_logging_server_settings_updates_enabled", id: "server-settings-log-settings-checkbox", kind: "bool", tab: "log" },
    { key: "is_logging_kicks_and_bans_enabled", id: "server-settings-log-kicks-bans-checkbox", kind: "bool", tab: "log" },
    { key: "is_logging_client_disconnects_enabled", id: "server-settings-log-disconnects-checkbox", kind: "bool", tab: "log" },
    { key: "is_logging_socket_opens_and_closes_enabled", id: "server-settings-log-sockets-checkbox", kind: "bool", tab: "log" },
    { key: "is_logging_failed_attempts_enabled", id: "server-settings-log-failed-checkbox", kind: "bool", tab: "log" },
    { key: "admin_log_max_size_mb", id: "server-settings-log-max-size-input", kind: "number", fallback: 10, tab: "log" },
    { key: "admin_log_retention_days", id: "server-settings-log-retention-select", kind: "number", fallback: 7, tab: "log" }
];

// stylesheet harvest and the Intl helper, both resolved once on first use
var country_code_cache = null;

var country_display_names = null;

var is_country_select_populated = false;

var server_settings_drafts =
{
    general:
    {
        loaded: false,
        dirty: false,
        revision: 0
    },
    log:
    {
        loaded: false,
        dirty: false,
        revision: 0
    }
};
var server_settings_pending_save = null;
var server_settings_password_revision = 0;
var server_settings_save_notice = "";
var server_settings_active_page = "general";
var server_settings_page_scroll = {};

/** @brief Prevents overlapping requests that share the server_settings_saved reply. */
function server_settings_tab__save_pending()
{
    return server_settings_pending_save !== null || server_music_snapshot_action !== null
        || (typeof server_emojis !== "undefined" && server_emojis.saving);
}

/** @brief Keeps the shared save button and its feedback outside the scrolling settings. */
function server_settings_tab__refresh_save_state()
{
    let pending = server_settings_tab__save_pending();
    let fields = document.getElementById("server-settings-general-settings-container");
    let save = document.getElementById("save-server-settings-button");
    let log_save = document.getElementById("server-settings-log-save-button");
    let status = document.getElementById("server-settings-save-status");

    if (fields)
    {
        fields.disabled = !server_settings_drafts.general.loaded;
    }
    for (let field of SERVER_SETTINGS_FIELDS)
    {
        if (field.tab === "log")
        {
            document.getElementById(field.id).disabled = !server_settings_drafts.log.loaded;
        }
    }
    if (save)
    {
        save.disabled = !server_settings_drafts.general.loaded || pending;
    }
    if (log_save)
    {
        log_save.disabled = !server_settings_drafts.log.loaded || pending;
    }
    if (status)
    {
        status.textContent = !server_settings_drafts.general.loaded ? "Loading settings..."
            : pending ? "Saving..." : server_settings_save_notice
                || (server_settings_drafts.general.dirty ? "Unsaved changes" : "");
    }
    if (typeof server_emojis__render_admin === "function")
    {
        server_emojis__render_admin();
    }
}

/** @brief Records edits so a later settings reply cannot overwrite an unfinished draft. */
function server_settings_tab__mark_dirty(tab, passwords = false)
{
    let draft = server_settings_drafts[tab];
    draft.dirty = true;
    draft.revision++;
    if (passwords)
    {
        server_settings_password_revision++;
    }
    server_settings_save_notice = "";
    server_settings_tab__refresh_save_state();
}

/** @brief Serializes settings saves and remembers which edits the request contains. */
function server_settings_tab__begin_save(tab, settings)
{
    let draft = server_settings_drafts[tab];
    if (!draft.loaded || server_settings_tab__save_pending())
    {
        return false;
    }
    server_settings_pending_save =
    {
        tab: tab,
        revision: draft.revision,
        password_revision: server_settings_password_revision,
        password_count: Array.isArray(settings.connection_passwords) ? settings.connection_passwords.length : null
    };
    server_settings_save_notice = "";
    server_settings_tab__refresh_save_state();
    return true;
}

/** @brief Discards connection-specific drafts and clears typed passwords after disconnect. */
function server_settings_tab__reset_drafts()
{
    for (let draft of Object.values(server_settings_drafts))
    {
        draft.loaded = false;
        draft.dirty = false;
        draft.revision = 0;
    }
    server_settings_pending_save = null;
    server_music_snapshot_action = null;
    server_settings_password_revision = 0;
    server_settings_save_notice = "";
    for (let id of ["server-encryption-password-rows", "server-music-snapshot-key-rows"])
    {
        let rows = document.getElementById(id);
        for (let input of rows.querySelectorAll("input"))
        {
            input.value = "";
        }
        rows.replaceChildren();
    }
    document.getElementById("server-encryption-replace-passwords").checked = false;
    document.getElementById("server-encryption-password-editor").hidden = true;
    document.getElementById("server-save-music-bots").checked = true;
    server_settings_tab__refresh_save_state();
}

/** @brief Tracks edits in the persistent controls, including dynamically added password rows. */
function server_settings_tab__setup()
{
    let fields = document.getElementById("server-settings-general-settings-container");
    let log = document.getElementById("server-settings-log-container");
    let picker = document.getElementById("server-settings-page-select");
    let edited = function(event)
    {
        if (event.target.closest("#server-music-snapshot-key-rows") || event.target.id === "server-banner-file-input")
        {
            return;
        }
        server_settings_tab__mark_dirty("general", event.target.id === "server-encryption-replace-passwords"
            || event.target.closest("#server-encryption-password-rows") !== null);
    };
    fields.addEventListener("input", edited);
    fields.addEventListener("change", edited);
    picker.addEventListener("change", function()
    {
        for (let item of document.getElementsByClassName("server-settings-tab-li"))
        {
            if (server_settings_tab__navigation_value(item) === picker.value)
            {
                item.click();
                break;
            }
        }
    });
    log.addEventListener("input", function(event)
    {
        if (SERVER_SETTINGS_FIELDS.some(function(field)
        {
            return field.tab === "log" && field.id === event.target.id;
        }))
        {
            server_settings_tab__mark_dirty("log");
        }
    });
    log.addEventListener("change", function(event)
    {
        if (SERVER_SETTINGS_FIELDS.some(function(field)
        {
            return field.tab === "log" && field.id === event.target.id;
        }))
        {
            server_settings_tab__mark_dirty("log");
        }
    });
    g_connection_status_listeners.push(function(status)
    {
        if (status.state !== "connected")
        {
            server_settings_tab__reset_drafts();
        }
    });
    server_settings_tab__refresh_save_state();
}

/** @brief Maps the sidebar entries to the same sections in the narrow-screen dropdown. */
function server_settings_tab__navigation_value(item)
{
    let management =
    {
        "1": "groups",
        "6": "emojis",
        "3": "bans",
        "5": "log"
    };
    return item.getAttribute("data-settings-page") || management[item.getAttribute("data-action")];
}

/** @brief Keeps the dropdown on the open section after navigation or a screen resize. */
function server_settings_tab__sync_navigation()
{
    let selected = document.querySelector(".server-settings-tab-li-selected");
    if (selected)
    {
        document.getElementById("server-settings-page-select").value = server_settings_tab__navigation_value(selected);
    }
}

/** @brief Switches settings pages without rebuilding controls or requesting replacement values. */
function server_settings_tab__show_page(page)
{
    let fields = document.getElementById("server-settings-general-settings-container");
    let target = document.getElementById("server-settings-page-" + page);
    if (!target)
    {
        return false;
    }
    for (let section of fields.querySelectorAll(".server-settings-page"))
    {
        section.hidden = section !== target;
    }
    server_settings_active_page = page;
    fields.scrollTop = server_settings_page_scroll[page] || 0;
    server_settings_tab__refresh_save_state();
    return true;
}

/**
 * @brief fills the two default theme selects with the themes this page contains, once. the list
 *        is read from the theme buttons of the local settings, so a theme added to the page shows
 *        up here without a second list. the first entry (an empty name) leaves the choice to the page
 *
 * @return void
 */
function server_settings_tab__fill_theme_selects()
{
    let desktop_select = document.getElementById("server-default-theme");
    let mobile_select = document.getElementById("server-default-mobile-theme");
    let theme_buttons = document.getElementsByClassName("choose-theme-item");
    let name = "";
    let label = "";

    // fill only once
    if (desktop_select == null || mobile_select == null || desktop_select.options.length > 0)
    {
        return;
    }

    // first entry
    desktop_select.appendChild(new Option("the page's own default", ""));
    mobile_select.appendChild(new Option("the page's own default", ""));

    // one entry per theme
    for (let i = 0; i < theme_buttons.length; i++)
    {
        name = theme_buttons[i].getAttribute("data-name");
        label = theme_buttons[i].textContent;
        desktop_select.appendChild(new Option(label, name));

        // a phone only accepts the themes made for a phone (UI.is_theme_allowed_on_this_device)
        if (theme_buttons[i].getAttribute("data-mobile") === "true")
        {
            mobile_select.appendChild(new Option(label, name));
        }
    }
}

/**
 * @brief fills the tab's inputs from a server_settings_values message; a number the server left out shows its fallback
 *
 * @param object values -> the message body
 *
 * @return void
 */
function server_settings_tab__apply_server_settings_values_to_tab(values)
{
    if (typeof server_info__apply_admin_settings === "function")
    {
        server_info__apply_admin_settings(values);
    }
    server_settings_tab__fill_theme_selects();
    for (let i = 0; i < SERVER_SETTINGS_FIELDS.length; i++)
    {
        let field = SERVER_SETTINGS_FIELDS[i];
        let element = document.getElementById(field.id);

        if (server_settings_drafts[field.tab].dirty
            || (server_settings_pending_save !== null && server_settings_pending_save.tab === field.tab))
        {
            continue;
        }

        if (field.kind == "bool")
        {
            element.checked = (typeof values[field.key] === "boolean") ? values[field.key] : (field.fallback === true);
        }
        else if (field.kind == "http-mode")
        {
            element.value = values[field.key] === true ? "blank" : "stop";
        }
        else if (field.kind == "select")
        {
            element.value = (typeof values[field.key] === "string") ? values[field.key] : field.fallback;
        }
        else
        {
            element.value = (typeof values[field.key] === "number") ? values[field.key] : field.fallback;
        }
    }
    server_settings_drafts.general.loaded = true;
    server_settings_drafts.log.loaded = true;
    server_settings_tab__refresh_channel_key_reuse_options();
    server_settings_tab__refresh_http_options();
    document.getElementById("server-http-port").textContent = "HTTP port: " + values.http_port + ". Changes apply to new page requests.";
    server_settings_tab__initialize_encryption(values.connection_password_count);
    server_settings_tab__apply_music_snapshot_status(values);
    server_settings_tab__refresh_save_state();
}

/**
 * @brief Shows the channel size and event choices when key reuse is enabled.
 */
function server_settings_tab__refresh_channel_key_reuse_options()
{
    let checkbox = document.getElementById("server-settings-channel-key-reuse-enabled");
    let options = document.getElementById("server-settings-channel-key-reuse-options");
    checkbox.onchange = server_settings_tab__refresh_channel_key_reuse_options;
    options.hidden = !checkbox.checked;
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
        else if (field.kind == "http-mode")
        {
            settings[field.key] = element.value === "blank";
        }
        else if (field.kind == "select")
        {
            settings[field.key] = element.value;
        }
        else
        {
            let number = parseInt(element.value);
            number = isNaN(number) ? field.fallback : number;
            if (field.min != null && field.max != null)
            {
                number = Math.min(field.max, Math.max(field.min, number));
                element.value = number;
            }
            settings[field.key] = number;
        }
    }

    settings.include_music_bots = document.getElementById("server-save-music-bots").checked;
    if (tab === "general" && document.getElementById("server-encryption-replace-passwords").checked)
    {
        settings.connection_passwords = [];
        for (let row of document.getElementById("server-encryption-password-rows").children)
        {
            let password = row.querySelector("input").value;
            if (new TextEncoder().encode(password).length >= 256)
            {
                document.getElementById("server-encryption-result").textContent = "Each password must fit in 255 UTF-8 bytes.";
                return null;
            }
            settings.connection_passwords.push({ type: row.querySelector("select").value, password: password });
        }
    }
    return settings;
}

/**
 * @brief the audio switch shows or hides the "audio connections" select below it
 *
 * @return void
 */
function server_settings_tab__refresh_audio_connections_row_visibility()
{
    let audio_checkbox = document.getElementById("server-settings-general-enable-audio");
    let row = document.getElementById("server-settings-general-audio-connections-row");

    if (audio_checkbox == null || row == null)
    {
        return;
    }

    row.style.display = (audio_checkbox.checked == true) ? "" : "none";
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
    let previous_is_connection_check_enforced = g_server_policy.is_connection_check_enforced;
    let preference = null;
    let previous_key_policy = keys__channel_key_reuse_policy_signature(g_server_policy);

    for (preference of Object.keys(g_attachment_receive_preferences))
    {
        g_server_policy["is_" + preference.replace(/^block_/, "blocking_") + "_active"] = data["is_" + preference.replace(/^block_/, "blocking_") + "_active"] === true;
    }
    chat_files__refresh_receive_preferences_controls();

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

    // The server also starts a fresh waiting period when checks are enabled again.
    // A long pause while they were disabled must not cause an immediate reconnect.
    if (previous_is_connection_check_enforced === false && g_server_policy.is_connection_check_enforced === true)
    {
        g_connection_check.last_response_timestamp = new Date().valueOf();
    }

    if (previous_key_policy !== keys__channel_key_reuse_policy_signature(g_server_policy))
    {
        // Refresh once so stricter settings also apply to keys already in use.
        keys__cancel_channel_key_creation();
        if (g_is_authenticated)
        {
            keys__create_and_send_new_channel_keys();
        }
    }

    groups__apply_server_mode();
    server_settings_tab__apply_rename_policy_to_ui();
    link_previews__apply_policy();
    chat_files__apply_file_upload_policy_to_ui();
    chat_files__apply_chat_picture_policy_to_ui();
    video__apply_policy_to_ui();
    audio_transport__select_mode();
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

    let may_rename = (g_server_policy.is_client_rename_allowed == true) || (g_is_local_client_admin == true);

    rename_input.readOnly = (may_rename == false);
    rename_input.title = (may_rename == false) ? "renames are disabled on this server" : "";
}

/**
 * @brief the icon upload loop: sends the next queued icon unless one is still being sent
 *        the reply handler clears g_icon_upload_in_flight_base64 and calls this again for the rest of the queue
 *
 * @return void
 */
function server_settings_tab__send_next_queued_icon_upload()
{
    if (!groups__has_permission("manage_icons"))
    {
        return;
    }
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
            server_settings_tab__mark_dirty("general");
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
    server_settings_tab__mark_dirty("general");
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
    if (server_settings_drafts.general.dirty
        || (server_settings_pending_save !== null && server_settings_pending_save.tab === "general"))
    {
        return;
    }
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

function server_settings_tab__initialize_encryption(count)
{
    let toggle = document.getElementById("server-encryption-replace-passwords");
    let editor = document.getElementById("server-encryption-password-editor");
    document.getElementById("server-encryption-password-count").textContent =
        String(Number.isInteger(count) ? count : 0) + " connection password(s) currently configured.";
    toggle.onchange = function()
    {
        editor.hidden = !toggle.checked;
    };
    document.getElementById("server-encryption-add-password").onclick = server_settings_tab__add_connection_password;
}

function server_settings_tab__add_connection_password()
{
    server_settings_tab__add_password_row(document.getElementById("server-encryption-password-rows"));
}

function server_settings_tab__add_password_row(rows)
{
    if (rows.children.length >= 100)
    {
        return;
    }
    let row = document.createElement("div");
    row.className = "server-settings-general-row";
    let cipher = document.createElement("select");
    cipher.setAttribute("aria-label", "Password cipher");
    let labels = ["AES-256", "ChaCha20", "Camellia-256", "Twofish-256", "Serpent-256", "Trivium-80 (fixed IV compatibility)"];
    for (let index = 0; index < CONNECTION_CIPHERS.length; index++)
    {
        let option = document.createElement("option");
        option.value = CONNECTION_CIPHERS[index];
        option.textContent = labels[index];
        cipher.appendChild(option);
    }
    let password = document.createElement("input");
    password.type = "password";
    password.autocomplete = "new-password";
    password.maxLength = 255;
    password.placeholder = "Password";
    password.setAttribute("aria-label", "Password");
    let remove = document.createElement("button");
    remove.type = "button";
    remove.textContent = "Remove";
    remove.onclick = function()
    {
        password.value = "";
        row.remove();
        if (rows.id === "server-encryption-password-rows")
        {
            server_settings_tab__mark_dirty("general", true);
        }
    };
    row.append(cipher, password, remove);
    rows.appendChild(row);
    if (rows.id === "server-encryption-password-rows")
    {
        server_settings_tab__mark_dirty("general", true);
    }
    password.focus();
}

function server_settings_tab__encryption_save_result(message)
{
    if (!G_HAS_DOM || message.type !== "server_settings_saved")
    {
        return;
    }
    server_settings_tab__apply_music_snapshot_status(message);
    if (server_music_snapshot_action !== null)
    {
        document.getElementById("server-music-snapshot-status").textContent = message.success === true
            ? (server_music_snapshot_action === "delete" ? "Saved music overwritten and cleared from disk. Songs in RAM remain available." : "Music restored to RAM. The saved copy remains on disk.")
            : (message.music_bot_snapshot_error || "Music snapshot action failed.");
        if (message.success !== true)
        {
            sounds__play("error");
        }
        server_music_snapshot_action = null;
        server_settings_tab__refresh_save_state();
        return;
    }
    let pending = server_settings_pending_save;
    if (pending === null)
    {
        return;
    }
    let draft = server_settings_drafts[pending.tab];
    server_settings_pending_save = null;
    sounds__play(message.success === true ? "you_edited_the_server" : "error");
    if (message.success === true && draft.revision === pending.revision)
    {
        draft.dirty = false;
    }
    if (pending.tab === "general")
    {
        server_settings_save_notice = message.success !== true
            ? "Settings could not be saved; your edits are still here."
            : draft.dirty ? "Settings saved. Newer edits are not saved yet." : "Settings saved.";
        document.getElementById("server-encryption-result").textContent = message.success === true
            ? "Settings saved. New connections use the new encryption settings; connected users keep their keys."
            : "Settings could not be saved; the previous settings remain in use.";
        if (message.success === true && pending.password_count !== null)
        {
            server_settings_tab__initialize_encryption(pending.password_count);
            if (pending.password_revision === server_settings_password_revision)
            {
                let rows = document.getElementById("server-encryption-password-rows");
                for (let input of rows.querySelectorAll("input"))
                {
                    input.value = "";
                }
                rows.replaceChildren();
                document.getElementById("server-encryption-replace-passwords").checked = false;
                document.getElementById("server-encryption-password-editor").hidden = true;
            }
        }
    }
    server_settings_tab__refresh_save_state();
}

function server_settings_tab__refresh_http_options()
{
    let enabled = document.getElementById("server-http-enabled");
    enabled.onchange = server_settings_tab__refresh_http_options;
    document.getElementById("server-http-disable-options").hidden = enabled.checked;
}

var server_music_snapshot_action = null;

function server_settings_tab__apply_music_snapshot_status(values)
{
    if (typeof values.music_bot_snapshot_present !== "boolean")
    {
        return;
    }
    let status = document.getElementById("server-music-snapshot-status");
    status.textContent = values.music_bot_snapshot_error || (values.music_bot_snapshot_present
        ? "Saved songs and playlists remain on disk until replaced by another music save or cleared with Clear from disk."
        : "No saved music snapshot is on disk. Save server settings to save the songs and playlists.");
    status.className = values.music_bot_snapshot_error ? "server-settings-general-warning" : "server-settings-general-help";
    status.setAttribute("role", values.music_bot_snapshot_error ? "alert" : "status");
    document.getElementById("server-music-snapshot-recovery").hidden = !values.music_bot_snapshot_present;
    document.getElementById("server-music-snapshot-retry-controls").hidden = values.music_bot_snapshot_can_restore !== true;
    document.getElementById("server-music-snapshot-retry").disabled = values.music_bot_snapshot_can_restore !== true;
    document.getElementById("server-music-snapshot-add-key").onclick = function()
    {
        server_settings_tab__add_password_row(document.getElementById("server-music-snapshot-key-rows"));
    };
    document.getElementById("server-music-snapshot-retry").onclick = function()
    {
        server_settings_tab__music_snapshot_action("restore");
    };
    document.getElementById("server-music-snapshot-delete").onclick = function()
    {
        if (window.confirm("Overwrite and clear the saved music from disk? Songs already in RAM remain available. Unrestored songs will be lost."))
        {
            server_settings_tab__music_snapshot_action("delete");
        }
    };
}

function server_settings_tab__music_snapshot_action(action)
{
    if (server_settings_tab__save_pending())
    {
        return;
    }
    let message = {type: "save_server_settings", music_bot_snapshot_action: action};
    let rows = document.getElementById("server-music-snapshot-key-rows");
    if (action === "restore" && rows.children.length !== 0)
    {
        message.music_bot_snapshot_keys = [];
        for (let row of rows.children)
        {
            let password = row.querySelector("input").value;
            if (new TextEncoder().encode(password).length >= 256)
            {
                document.getElementById("server-music-snapshot-status").textContent = "Each recovery password must fit in 255 UTF-8 bytes.";
                return;
            }
            message.music_bot_snapshot_keys.push({type: row.querySelector("select").value, password: password});
        }
    }
    server_music_snapshot_action = action;
    server_settings_tab__refresh_save_state();
    connection__send_message_object({message: message});
    for (let input of rows.querySelectorAll("input"))
    {
        input.value = "";
    }
    rows.replaceChildren();
}
