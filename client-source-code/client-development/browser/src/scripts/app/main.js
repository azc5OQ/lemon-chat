// main.js is embedded in template.html along with the other client files, and in the node bundle
// it is the startup: main__window_onload wires the page (buttons, keys, panels, local settings), and the
// export seam at the end hands the node runtime what it needs
// it also closes the closure that aes-js.js opens, so it stays the last include

// state private to this file
// true once java has answered whether anyone is looking. java watches the activity while the
// loopback socket only knows whether anyone is connected, so java outranks it
var node_ui_visibility_from_host = false;

/**
 * @brief global mousedown: closes the chat-message context menu, and every other context menu
 *        unless the click landed on a menu item (so the item's own click handler still runs)
 *
 * @param object event -> the mouse event
 *
 * @return void
 */
function main__document_onmousedown(event)
{
    UI.delete_chat_message_contextmenu();

    let is_channel_list_contextmenu_delete_needed = !event.target.classList.contains("context-menu-item");

    if (is_channel_list_contextmenu_delete_needed)
    {
        UI.delete_contextmenus(true);
    }
}

/**
 * @brief the push-to-talk key (Q unless changed in local settings): held down = speaking
 *        in continuous mode the mic is toggled by the button instead, so the key does nothing
 *
 * @param object event -> the keyboard event
 *
 * @return void
 */
function main__document_onkeydown(event)
{
    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    if (event.which == g_push_to_talk_key_code)
    {
        voice__process_start_sending_audio();
    }
}

/**
 * @brief releasing the push-to-talk key stops sending audio
 *
 * @param object event -> the keyboard event
 *
 * @return void
 */
function main__document_onkeyup(event)
{
    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    if (event.which == g_push_to_talk_key_code)
    {
        // the encoder clear moved into voice__process_stop_sending_audio_now: clearing here
        // would wipe the release-hangover tail that is still being captured
        voice__process_stop_sending_audio();
    }
}

/**
 * @brief the android wrapper's simple/advanced mode drives the theme, both ways
 *        simple locks the messenger look; advanced restores the user's saved theme (or the device
 *        default). called at startup
 *
 * @return void
 */
function main__apply_theme_for_app_mode()
{
    // simple = the bluebell messenger look, locked
    if (g_android_app_mode == "simple")
    {
        UI.apply_theme("bluebell", false);
        return;
    }

    // advanced = the full mobile interface. honor a theme the user picked manually (so
    // their choice persists), otherwise fall back to default-mobile
    if (g_android_app_mode == "advanced")
    {
        let saved_theme = null;
        saved_theme = utils__storage_get("lemon_theme");

        if (saved_theme != null && saved_theme != "bluebell" && UI.is_theme_allowed_on_this_device(saved_theme))
        {
            UI.apply_theme(saved_theme, false);
        }
        else
        {
            UI.apply_theme("default-mobile", false);
        }
    }
}

/**
 * @brief stops event bubbling; wired where a click must not reach parent handlers
 *
 * @param object event -> the event to stop
 *
 * @return void
 */
function main__stop_propagation(event)
{
    event.stopPropagation();
}

/**
 * @brief mirrors the three toggle states onto css classes of their toolbar buttons
 *        .sfx-on / .mic-on / .chat-hidden, which themes with state-specific icons (oldschool)
 *        style with !important
 *
 * @return void
 */
function main__sync_toolbar_state_classes()
{
    let sfx = document.getElementById("sound-effects-button");
    let mic = document.getElementById("microphone-always-broadcasting-audio-button");
    let hide = document.getElementById("hide-chat-button");
    if (sfx != null) { sfx.classList.toggle("sfx-on", g_are_sound_effects_enabled == true); }
    if (mic != null) { mic.classList.toggle("mic-on", g_is_microphone_always_on == true); }
    if (hide != null) { hide.classList.toggle("chat-hidden", g_is_chat_hidden == true); }
}

/**
 * @brief the page entry point
 *        detects the android webview and touch devices, applies the predefined server
 *        autoconnect config, sets up ui visibility, gestures and every element's event handlers
 *
 * @return promise ignored by window.onload; resolves once the page is wired
 */
async function main__window_onload()
{
    if (typeof Android !== 'undefined')
    {
        g_is_running_in_android_webview = true;
        g_is_client_running_under_touch_device = true;
        g_is_autoconnect_without_user_action_active = false;
        g_are_server_details_predefined = false;
    }

    // touch detection lives in platform-detection.js; the android webview force above stays
    if (utils__detect_touch_device() == true)
    {
        g_is_client_running_under_touch_device = true;
    }

    if (g_is_running_in_android_webview == true)
    {
        // in android, connection settings are stored in its own container..
        // these ui elements are not needed
        document.getElementById("add-key-button").style.visibility = "hidden";
        document.getElementById("connect-form-sub-container-1").style.visibility = "hidden";
        document.getElementById("connect-form-sub-container-2").style.visibility = "hidden";
    }

    // web: if the server baked connection details into the page, enable autoconnect before the form-visibility logic below runs; hide only the connect form (ip/port), leaving the loading/connecting status visible so the page isn't blank while it connects
    if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.port && g_is_running_in_android_webview == false)
    {
        g_are_server_details_predefined = true;
        g_is_autoconnect_without_user_action_active = true;
        g_autoconnect_details = {
            host: window.location.hostname,
            port: window.__SERVER_CONFIG__.port,
            keys: window.__SERVER_CONFIG__.keys,
            wss_port: window.__SERVER_CONFIG__.wss_port
        };
        document.getElementById("connect-form-sub-container-1").style.display = "none";
        document.getElementById("connect-form-sub-container-2").style.display = "none";
    }

    if (g_is_running_in_android_webview == false)
    {
        // the address is baked in (injected config, or set by hand in the source), so the fields the
        // bookmarks fill are never consulted: hidden in every theme, and it stays hidden on the
        // connect screen after a disconnect
        if (g_are_server_details_predefined == true)
        {
            document.getElementById("server-bookmarks-container").style.display = "none";
        }

        if (g_are_server_details_predefined == false)
        {
            document.getElementById("connect-form-sub-container-1").style.visibility = "visible";
            document.getElementById("connect-form-sub-container-2").style.visibility = "visible";
            document.getElementById("add-key-button").style.visibility = "visible";
            document.getElementById("connect-button").style.visibility = "visible";
        }

        if (g_are_server_details_predefined == true && g_is_autoconnect_without_user_action_active == false)
        {
            document.getElementById("connect-button").style.visibility = "visible";
        }

        if (g_is_autoconnect_without_user_action_active == true)
        {
            document.getElementById("import-identity-button").style.display = "none";
        }
    }

    asm = create_opus_webassembly_instance(); // do not delete this line

    g_textarea_log = document.getElementById("textarea-log");

    // member-strip gestures: strip themes tap the clones in the right-pane member list, which are
    // rebuilt on every sync, so delegated listeners forward tap (open chat / show channel chat) and
    // hold (row menu / channel switch list) to the real tree rows; touch and mouse both register

    function member_strip_show_channel_switch_menu(click_x, click_y)
    {
        UI.delete_contextmenus();

        let menu_items_html = "";
        for (let i = 0; i < g_channel_list.length; i++)
        {
            let channel = g_channel_list[i];
            let lock_html = (channel.is_using_password == true) ? "🔒 " : "";
            let current_html_class = (parseInt(channel.channel_id) == parseInt(g_current_channel_id)) ? " context-menu-item-disabled" : "";
            // context-menu-item keeps main__document_onmousedown from closing the menu
            // before the item's own click handler can run
            menu_items_html += "<p class='context-menu-item channel-switch-menu-item" + current_html_class + "' data-switch-channel-id='" + channel.channel_id + "'>" + lock_html + chat__sanitize_string(channel.name) + "</p>\n";
        }

        let menu_html = "<div class=\"channel-list-context-menu\" style=\"top: " + click_y + "px; left: " + click_x + "px;\">\n\
                            <div class='channel-list-context-menu-background'>\n\
                            </div>\n\
                            <div class='channel-list-context-menu-items'>\n\
                                " + menu_items_html + "\n\
                            </div>\n\
                        </div>";

        document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", menu_html);

        let switch_items = document.getElementsByClassName("channel-switch-menu-item");
        for (let i = 0; i < switch_items.length; i++)
        {
            switch_items[i].onclick = function()
            {
                let switch_channel_id = this.getAttribute("data-switch-channel-id");
                UI.delete_contextmenus(true);

                // same flow as double clicking the channel in the tree: handles the
                // current-channel guard and the password dialog for locked channels
                let name_element = document.querySelector('#channel-list-container .single-channel[data-channel-id="' + switch_channel_id + '"] [data-channel-name-id]');
                if (name_element != null)
                {
                    UI.single_channel_doubleclick_join({ stopPropagation: function() {}, target: name_element });
                }
            };
        }
    }

    // true when the gesture landed on a circle and was handled, so the caller stops the event and
    // main__document_onmousedown does not close the menu the gesture just opened
    function member_strip_forward(target_element, click_x, click_y, is_short_click)
    {
        let channel_circle = target_element.closest(".member-list-channel");
        if (channel_circle != null)
        {
            if (is_short_click == true)
            {
                // bring the channel chat back on screen - same as tapping its pill. a pill
                // fresh from the html has no handler yet - give it the standard one
                let channel_pill = document.querySelector('.chat-context-selector[data-chat-context-selector-id="channel-' + channel_circle.getAttribute("data-member-list-channel-id") + '"]');
                if (channel_pill != null)
                {
                    if (channel_pill.onclick == null)
                    {
                        channel_pill.onclick = UI.chat_context_selector_onclick;
                    }

                    channel_pill.click();
                }
            }
            else
            {
                member_strip_show_channel_switch_menu(click_x, click_y);
            }
            return true;
        }

        let clone = target_element.closest(".member-list-client");
        if (clone == null) { return false; }

        // offline contacts have no live row to forward to. a HOLD shows what we know about
        // them; a TAP opens their conversation when the server keeps messages for people
        // who are away (we can tell: we were given their public key), else the info popup
        if (clone.classList.contains("offline-contact") == true)
        {
            let offline_alias_element = clone.querySelector(".client-alias");
            let offline_alias = (offline_alias_element != null) ? offline_alias_element.textContent : "";
            let offline_contact = channel_tree__get_stored_client_by_alias(offline_alias);

            // a TAP opens the conversation like it does for anybody else - the chat itself
            // says whether it can actually be delivered. a HOLD shows their details.
            if (is_short_click == true && offline_contact != null)
            {
                UI.open_offline_chat_context(offline_contact);
            }
            else
            {
                UI.show_offline_contact_info_popup(offline_alias);
            }
            return true;
        }

        let strip_client_id = clone.getAttribute("data-connected-client-id");
        if (strip_client_id == null) { return false; }

        let tree_row = document.querySelector('#channel-list-container .connected-client[data-connected-client-id="' + strip_client_id + '"], #idle-clients-container .connected-client[data-connected-client-id="' + strip_client_id + '"]');
        if (tree_row == null) { return false; }

        // the row handler accepts an explicit target + coordinates, so the strip can
        // hand it the REAL tree row no matter which clone was tapped
        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, click_x, click_y, tree_row, is_short_click);
        return true;
    }

    let member_strip_body = document.getElementById("member-list-body");
    if (member_strip_body != null)
    {
        let member_strip_press_timer = null;
        let member_strip_long_press = false;
        let member_strip_moved = false;

        member_strip_body.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            member_strip_long_press = false;
            member_strip_moved = false;

            const target_element = event.target;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            member_strip_press_timer = window.setTimeout(function() {
                member_strip_long_press = true;
                member_strip_forward(target_element, click_x, click_y, false);
            }, 600);
        }, { passive: true });

        member_strip_body.addEventListener("touchmove", function() {
            // finger is scrolling the strip, not pressing a circle
            member_strip_moved = true;
            clearTimeout(member_strip_press_timer);
        }, { passive: true });

        member_strip_body.addEventListener("touchend", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            clearTimeout(member_strip_press_timer);

            if (member_strip_long_press == false && member_strip_moved == false)
            {
                member_strip_forward(event.target, event.changedTouches[0].clientX, event.changedTouches[0].clientY, true);
            }
        });

        member_strip_body.addEventListener("mousedown", function(event) {
            if (g_is_client_running_under_touch_device == true) { return; }

            // left opens, right = actions / channel list - same split the tree rows use
            if (event.which == 1 || event.which == 3)
            {
                if (member_strip_forward(event.target, event.clientX, event.clientY, event.which == 1) == true)
                {
                    event.stopPropagation();
                }
            }
        });
    }

    // holding a channel pill offers the channel switch list too, but only while a
    // theme hides the channel tree (the strip themes) - elsewhere the tree does that job
    let context_pill_container = document.getElementById("chat-context-selectors-container");
    if (context_pill_container != null)
    {
        function is_channel_tree_hidden()
        {
            let tree_pane = document.getElementById("space-devider1");
            return (tree_pane != null && getComputedStyle(tree_pane).display == "none");
        }

        function pill_press_target(event_target)
        {
            let pill = event_target.closest('.chat-context-selector[data-chat-context-selector-type="channel"]');
            return pill;
        }

        let pill_press_timer = null;
        let pill_long_press = false;

        context_pill_container.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }
            if (pill_press_target(event.target) == null || is_channel_tree_hidden() == false) { return; }

            pill_long_press = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            pill_press_timer = window.setTimeout(function() {
                pill_long_press = true;
                member_strip_show_channel_switch_menu(click_x, click_y);
            }, 600);
        }, { passive: true });

        context_pill_container.addEventListener("touchmove", function() {
            clearTimeout(pill_press_timer);
        }, { passive: true });

        context_pill_container.addEventListener("touchend", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            clearTimeout(pill_press_timer);

            // a long press must not ALSO fire the pill's own click
            if (pill_long_press == true)
            {
                event.preventDefault();
            }
        });

        context_pill_container.addEventListener("mousedown", function(event) {
            if (g_is_client_running_under_touch_device == true) { return; }

            if (event.which == 3 && pill_press_target(event.target) != null && is_channel_tree_hidden() == true)
            {
                event.stopPropagation(); // or main__document_onmousedown closes it in the same event
                member_strip_show_channel_switch_menu(event.clientX, event.clientY);
            }
        });
    }

    // long press on an own message opens the delete/edit menu: phones never send the right-click
    // mousedown desktop uses, so a 600 ms hold synthesizes the same event on the same element
    let chat_context_container_element = document.getElementById("chat-context-container");
    if (chat_context_container_element != null)
    {
        let message_press_timer = null;
        let message_press_moved = false;

        chat_context_container_element.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false) { return; }

            let message_p = event.target.closest(".local-single-chat-message-content-p, .local-client-chat-picture-img");
            if (message_p == null) { return; }

            message_press_moved = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            message_press_timer = window.setTimeout(function() {
                if (message_press_moved == true) { return; }
                // bubbles:false is load-bearing: a bubbling mousedown would reach main__document_onmousedown,
                // whose menu cleanup would delete the menu created by this very dispatch
                message_p.dispatchEvent(new MouseEvent("mousedown", {
                    bubbles: false,
                    cancelable: true,
                    button: 2,
                    clientX: click_x,
                    clientY: click_y
                }));
            }, 600);
        }, { passive: true });

        chat_context_container_element.addEventListener("touchmove", function() {
            // finger is scrolling the chat, not holding a message
            message_press_moved = true;
            clearTimeout(message_press_timer);
        }, { passive: true });

        chat_context_container_element.addEventListener("touchend", function() {
            clearTimeout(message_press_timer);
        });
    }

    // member-strip preferences, persisted like the theme, as body classes and css variables:
    // orientation (top row or side rail), neighbors only, the own-actions menu, the circle size
    let saved_strip_vertical = null;
    let saved_strip_scale = null;
    let saved_strip_neighbors = null;
    saved_strip_vertical = utils__storage_get("lemon_strip_vertical");
    saved_strip_scale = utils__storage_get("lemon_strip_scale");
    saved_strip_neighbors = utils__storage_get("lemon_strip_neighbors");

    if (saved_strip_vertical == "1") { document.body.classList.add("msgr-vertical"); }
    if (saved_strip_neighbors == "1") { document.body.classList.add("msgr-neighbors-only"); }

    // on unless it was turned off. the badge only appears for people with audio
    // off, so leaving it on costs nothing until there is something worth saying
    let saved_audio_availability = null;
    saved_audio_availability = utils__storage_get("lemon_audio_availability");

    if (saved_audio_availability != "0") { document.body.classList.add("msgr-audio-availability"); }

    function apply_strip_scale(circle_px)
    {
        document.body.style.setProperty("--msgr-circle", circle_px + "px");
        document.body.style.setProperty("--msgr-item", (circle_px + 16) + "px");
        document.body.style.setProperty("--msgr-strip", (circle_px + 52) + "px");
        document.body.style.setProperty("--msgr-name", (circle_px <= 46 ? 10 : (circle_px <= 56 ? 11 : 12)) + "px");
    }

    // legacy values were the steps "1"/"2"/"3"; anything bigger is pixels
    let restored_circle_px = 46;
    if (saved_strip_scale != null)
    {
        if (saved_strip_scale == "2") { restored_circle_px = 54; }
        else if (saved_strip_scale == "3") { restored_circle_px = 62; }
        else if (parseInt(saved_strip_scale) >= 38) { restored_circle_px = parseInt(saved_strip_scale); }
    }
    apply_strip_scale(restored_circle_px);

    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin",
        "<input type='button' id='msgr-orientation-button' title='people strip: switch between top row and side rail'>" +
        "<input type='button' id='msgr-neighbors-button' title='show only people in your channel'>" +
        "<input type='button' id='msgr-me-button' title='you: avatar and account actions'>" +
        "<input type='range' id='msgr-scale-slider' min='38' max='64' step='2' value='" + restored_circle_px + "' title='people size'>");

    document.getElementById("msgr-orientation-button").onclick = function()
    {
        let vertical_now = document.body.classList.toggle("msgr-vertical");
        utils__storage_set("lemon_strip_vertical", vertical_now ? "1" : "0");
    };

    document.getElementById("msgr-neighbors-button").onclick = function()
    {
        let neighbors_now = document.body.classList.toggle("msgr-neighbors-only");
        utils__storage_set("lemon_strip_neighbors", neighbors_now ? "1" : "0");
    };

    // own actions (set avatar / delete avatar): the same menu a long press on
    // the own row would open - the row itself is hidden in the strip theme
    document.getElementById("msgr-me-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");
        if (local_row == null) { return; }

        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, window.innerWidth - 190, 48, local_row, false);
    };

    document.getElementById("msgr-scale-slider").oninput = function()
    {
        let circle_px = parseInt(this.value);
        apply_strip_scale(circle_px);
        utils__storage_set("lemon_strip_scale", "" + circle_px);
    };

    // session info: its own icon in the bar, a small card with connection status,
    // ping (heartbeat round trip), traffic counters and session length. values
    // refresh once a second, but only while the card is open.
    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin",
        "<input type='button' id='msgr-session-button' title='connection and session info'>");

    document.body.insertAdjacentHTML("beforeend",
        "<div id='msgr-session-card'>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>status</span><span id='msgr-session-status'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>ping</span><span id='msgr-session-ping'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>sent</span><span id='msgr-session-sent'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>received</span><span id='msgr-session-received'></span></p>" +
            "<p class='msgr-session-row'><span class='msgr-session-label'>session</span><span id='msgr-session-uptime'></span></p>" +
        "</div>");

    function format_byte_count(byte_count)
    {
        if (byte_count >= 1048576) { return (byte_count / 1048576).toFixed(1) + " MB"; }
        if (byte_count >= 1024) { return (byte_count / 1024).toFixed(1) + " KB"; }
        return byte_count + " B";
    }

    function refresh_session_card()
    {
        let now = new Date().valueOf();
        let is_connection_alive = (g_is_authenticated == true && (g_connection_check.last_response_timestamp + g_connection_check.lost_threshold_ms) >= now);

        document.getElementById("msgr-session-status").textContent = is_connection_alive ? "connected" : "not connected";
        document.getElementById("msgr-session-status").style.color = is_connection_alive ? "#3ddc84" : "#e05a4e";
        document.getElementById("msgr-session-ping").textContent = (is_connection_alive && g_session.last_ping_ms >= 0) ? (g_session.last_ping_ms + " ms") : "-";
        document.getElementById("msgr-session-sent").textContent = format_byte_count(g_session.bytes_sent);
        document.getElementById("msgr-session-received").textContent = format_byte_count(g_session.bytes_received);

        let uptime_text = "-";
        if (is_connection_alive == true && g_session.connected_at > 0)
        {
            let total_seconds = Math.floor((now - g_session.connected_at) / 1000);
            uptime_text = Math.floor(total_seconds / 3600) + "h " + Math.floor((total_seconds % 3600) / 60) + "m " + (total_seconds % 60) + "s";
        }
        document.getElementById("msgr-session-uptime").textContent = uptime_text;
    }

    let session_card_refresh_timer = null;

    document.getElementById("msgr-session-button").onclick = function()
    {
        let card = document.getElementById("msgr-session-card");
        let opening = (card.classList.contains("msgr-session-card-open") == false);
        card.classList.toggle("msgr-session-card-open", opening);

        clearInterval(session_card_refresh_timer);
        if (opening == true)
        {
            refresh_session_card();
            session_card_refresh_timer = setInterval(refresh_session_card, 1000);
        }
    };

    // tapping anywhere else closes the card - it is a glance, not a screen
    document.addEventListener("mousedown", function(event) {
        let session_card = document.getElementById("msgr-session-card");
        if (session_card == null || session_card.classList.contains("msgr-session-card-open") == false) { return; }
        if (event.target.closest("#msgr-session-card") != null || event.target.closest("#msgr-session-button") != null) { return; }

        session_card.classList.remove("msgr-session-card-open");
        clearInterval(session_card_refresh_timer);
    });

    // grid layout engine: desktop only. touch keeps the legacy inline-block layout,
    // so there g_layout.grid_active stays false and every layout_* entry point no-ops
    g_layout.panels = {
        channels: document.getElementById("space-devider1"),
        chat: document.getElementById("space-devider2"),
        info: document.getElementById("space-devider-42"),
        input: document.getElementById("space-devider4")
    };
    if (g_is_client_running_under_touch_device == false)
    {
        layout__layout_load_saved_state();
        g_layout.grid_active = true;
        layout__layout_apply();

        document.getElementById("layout-edit-button").onclick = layout__layout_edit_toggle;
        document.getElementById("drag-resize-info").addEventListener("mousedown", function(e) {
            layout__layout_column_drag_start(e, "info");
        }, false);
        document.getElementById("drag-resize-channels").addEventListener("mousedown", function(e) {
            layout__layout_column_drag_start(e, "channels");
        }, false);

        // edit mode: capture-phase so a panel drag wins over every inner click handler
        document.getElementById("communication-system-container").addEventListener("mousedown", layout__layout_edit_mousedown, true);
        document.addEventListener("mousemove", layout__layout_edit_mousemove, false);
        document.addEventListener("mouseup", layout__layout_edit_mouseup, false);
    }
    else
    {
        document.getElementById("layout-edit-button").style.display = "none";
        document.getElementById("drag-resize-info").style.display = "none";
        document.getElementById("drag-resize-channels").style.display = "none";
    }

    document.addEventListener("keydown", main__document_onkeydown);
    document.addEventListener("mousedown", main__document_onmousedown);
    document.addEventListener("keyup", main__document_onkeyup);

    document.getElementById("channel-properties-edit").addEventListener("mousedown", main__stop_propagation);
    document.getElementById("background-container").addEventListener("mousedown", main__stop_propagation);

    document.getElementById("chat-input-container-send-chat-input").onclick = UI.send_chat_input_on_click;

    // the pills shipped in the html (the root channel's) have no handlers until a rebind loop
    // happens to run; bind them now so they work on a fresh session too - opening an offline
    // chat used to leave the channel pill (and the strip's channel circle) dead
    for (let x = 0; x < document.getElementsByClassName("chat-context-selector").length; x++)
    {
        document.getElementsByClassName("chat-context-selector")[x].onclick = UI.chat_context_selector_onclick;
    }
    for (let x = 0; x < document.getElementsByClassName("remove-chat-context-selector").length; x++)
    {
        document.getElementsByClassName("remove-chat-context-selector")[x].onclick = UI.chat_context_remove_onclick;
    }
    document.getElementById("connect-button").onclick = UI.connect_button_onclick;
    connection__wire_server_bookmarks();
    document.getElementById("chat-input-container-text-input").onkeyup = UI.chat_input_on_keyup;
    document.getElementById("choose_image_input").onchange = UI.choose_image_input;
    document.getElementById("choose-file-input").onchange = chat_files__choose_chat_file_input_onchange;
    document.getElementById("image-upload-preview-remove").onclick = chat_files__clear_pending_chat_picture;
    document.getElementById("server-settings-general-file-upload-max-size-input").oninput = chat_files__refresh_file_upload_size_warning;
    document.getElementById("server-settings-general-allow-file-uploads-checkbox").onchange = chat_files__refresh_file_upload_size_visibility;
    document.getElementById("server-settings-general-allow-pictures-checkbox").onchange = chat_files__refresh_picture_size_visibility;
    document.getElementById("server-settings-general-country-blocking-checkbox").onchange = server_settings_tab__refresh_country_blocking_visibility;
    document.getElementById("server-settings-general-display-flags-checkbox").onchange = server_settings_tab__refresh_hide_admin_flag_row_visibility;
    document.getElementById("server-settings-country-block-select").onchange = server_settings_tab__country_block_select_onchange;
    chat_files__setup_chat_file_drag_and_drop();
    chat_files__setup_chat_file_card_glow();
    chat__setup_public_key_expand();
    chat_files__apply_file_upload_policy_to_ui();
    document.getElementById("add-key-button").onclick = UI.add_key_button_on_click;
    document.getElementById("show-hide-log-button").onclick = UI.show_hide_log_on_click;
    document.querySelector("[data-chat-context-remove-selector-id='channel-0']").onclick = UI.chat_context_remove_onclick;
    document.getElementById("close-button").onclick = UI.channel_properties_closebutton_onclick;
    document.getElementById("create-channel-button").onclick = UI.create_channel_button_onclick;
    document.getElementById("edit-channel-button").onclick = UI.edit_channel_button_onclick;
    document.getElementById("channel-properties-limit-clients-checkbox").onchange = UI.refresh_channel_limit_input_visibility;
    document.getElementById("close-button-enter-password").onclick = UI.enter_channel_password_closebutton_onclick;
    document.getElementById("close-button-admin-password").onclick = UI.close_admin_password_context_button;
    document.getElementById("close-button-secret-identity-string").onclick = UI.close_button_secret_identity_string_onclick;
    document.getElementById("close-button-identity-string-use").onclick = UI.close_button_identity_string_use;
    document.getElementById("join-passworded-channel-button").onclick = UI.channel_join_button_onclick;
    document.getElementById("admin-password-send-button").onclick = UI.admin_password_send_button_onclick;
    document.getElementById("clear-chat-button").onclick = UI.clear_chat_button_onclick;
    document.getElementById("hide-chat-button").onclick = UI.hide_chat_container;
    document.getElementById("show-admin-password-entry").onclick = UI.enter_admin_password_button_onclick;
    document.getElementById("choose-song-file-input").onchange = UI.choose_song_file_input_onchange;
    document.getElementById("choose_icon_input").onchange = UI.choose_icon_input_onchange;
    document.getElementById("drag-resize-chat").addEventListener('mousedown', UI.drag_resize_chat_onclick, false);
    document.getElementById("stop-song").onclick = UI.stop_song_onclick;
    document.getElementById("add-new-tag-to-server-button").onclick = UI.add_new_tag_to_server_button_onlick;
    document.getElementById("save-server-settings-button").onclick = UI.save_server_settings_button_onclick;
    document.getElementById("server-settings-log-save-button").onclick = UI.server_settings_log_save_button_onclick;
    document.getElementById("server-settings-log-refresh-button").onclick = UI.server_settings_log_refresh_button_onclick;
    document.getElementById("server-settings-log-clear-button").onclick = UI.server_settings_log_clear_button_onclick;
    document.getElementById("enter-server-settings").onclick = UI.enter_server_settings_onclick;
    document.getElementById("channel-flatten-toggle-button").onclick = UI.channel_flatten_toggle_onclick;
    document.getElementById("show-secret-identity-string").onclick = UI.show_secret_identity_string_onclick;
    document.getElementById("export-identity-file-button").onclick = UI.export_identity_file_button_onclick;
    document.getElementById("choose-identity-file-button").onclick = UI.choose_identity_file_button_onclick;
    document.getElementById("identity-file-input").onchange = UI.identity_file_input_onchange;
    document.getElementById("identity-file-confirm-button").onclick = UI.identity_file_confirm_button_onclick;
    document.getElementById("identity-mode-toggle-link").onclick = UI.identity_mode_toggle_onclick;
    // dropping a .lmn file on the identity dialog behaves like picking it; stopPropagation keeps
    // the drop away from the chat-file handlers
    {
        let identity_dialog = document.getElementById("identity-string-use-container-enter");
        identity_dialog.addEventListener("dragover", function(drag_event) { drag_event.preventDefault(); drag_event.stopPropagation(); });
        identity_dialog.addEventListener("drop", function(drop_event)
        {
            drop_event.preventDefault();
            drop_event.stopPropagation();

            if (document.getElementById("identity-file-mode").style.display == "none")
            {
                UI.identity_mode_toggle_onclick(drop_event);
            }

            if (drop_event.dataTransfer != null && drop_event.dataTransfer.files != null && drop_event.dataTransfer.files.length > 0)
            {
                UI.identity_read_identity_file(drop_event.dataTransfer.files[0]);
            }
        });
    }
    document.getElementById("import-identity-button").onclick = UI.import_identity_button_onclick;
    // top bar: same passphrase dialog; entered while connected it disconnects, regenerates
    // the keypair from the passphrase and reconnects as that identity (see the confirm handler)
    document.getElementById("switch-identity-button").onclick = UI.import_identity_button_onclick;
    document.getElementById("identity-string-use-confirm-button").onclick = UI.identity_string_use_confirm_button;
    document.getElementById("close-button-poke-client").onclick = UI.close_button_poke_client_onclick;
    document.getElementById("close-button-client-info").onclick = UI.close_button_client_info_onclick;
    document.getElementById("close-button-set-new-username").onclick = UI.close_button_set_new_username_onclick;
    document.getElementById("close-button-server-settings").onclick = UI.close_button_server_settings_onclick;
    document.getElementById("poke-client-send-button").onclick = UI.poke_client_send_button_onclick;
    document.getElementById("set-alias-send-button").onclick = UI.set_alias_send_button_onclick;
    document.getElementById("close-button-set-alias").onclick = UI.close_button_set_alias_onclick;

    // channel edit form: clicking the icon box opens the shared icon picker for that channel.
    // no admin gate here: the picker always opens, the server rejects the request if not allowed
    document.getElementById("channel-properties-icon-box").onclick = function()
    {
        if (g_channel_properties_edit_channel_id != null)
        {
            UI.open_channel_icon_picker(g_channel_properties_edit_channel_id);
        }
    };

    // "remove icon" next to the box: same set_channel_icon request with no icon_id, which the
    // server treats as clearing it; its broadcast repaints the tree row and the edit form box
    document.getElementById("channel-properties-icon-remove-button").onclick = function()
    {
        if (g_channel_properties_edit_channel_id != null)
        {
            UI.send_set_channel_icon_request(g_channel_properties_edit_channel_id, "none");
        }
    };

    // adjust-volume dialog: close button + live slider (updates the worklet lane gain as it moves)
    document.getElementById("close-button-adjust-volume").onclick = function()
    {
        document.getElementById("adjust-volume-container").style.display = "none";
    };
    document.getElementById("adjust-volume-slider").oninput = function()
    {
        let target_client_id = this.getAttribute("data-target-client-id");
        let gain = parseInt(this.value) / 100;

        document.getElementById("adjust-volume-value-label").innerHTML = this.value + "%";

        if (target_client_id != null)
        {
            g_client_volume_by_id[target_client_id] = gain;
            audio__set_client_playback_volume(parseInt(target_client_id), gain);
        }
    };
    document.getElementById("set-new-username-send-button").onclick = UI.set_new_username_send_button_onclick;
    document.getElementById("chat-input-file-input-container").onmousedown = UI.chat_input_container_on_mousedown;
    document.getElementById("color-picker-input").addEventListener("input", UI.color_picker_oninput);
    document.getElementById("chat-input-font-size-range").addEventListener("input", UI.chat_input_font_size_range_oninput);
    document.getElementById("hide-show-flags-button").onclick = UI.hide_show_flags_button_onclick;
    document.getElementById("microphone-always-broadcasting-audio-button").onclick = UI.activate_continous_audio_broadcast_onclick;
    document.getElementById("sound-effects-button").onclick = UI.sound_effects_button_onclick;

    // local settings: device-only preferences, no admin involved. new messages only -
    // already-rendered ones keep whatever they had
    document.getElementById("local-settings-button").onclick = function()
    {
        let panel = document.getElementById("local-settings-container");
        document.getElementById("local-settings-show-avatars").checked = g_show_message_avatars;
        document.getElementById("local-settings-rsa-key-bits").value = String(g_rsa_key_bits);
        document.getElementById("local-settings-seen-indicator").checked = g_show_seen_indicator;
        document.getElementById("local-settings-send-seen").checked = g_send_seen_receipts;
        document.getElementById("local-settings-auto-scroll").checked = g_auto_scroll_chat_to_end;
        document.getElementById("local-settings-hide-mic").checked = g_hide_microphone_button;
        document.getElementById("local-settings-sound-effects").checked = g_are_sound_effects_enabled;
        document.getElementById("local-settings-country-flags").checked = (g_show_hide_toggle == false);
        render_mic_mode_controls();
        document.getElementById("local-settings-file-logging").checked = g_is_file_logging_enabled;
        document.getElementById("local-settings-show-log").checked = (document.getElementById("show-hide-log-button").style.display !== "none");
        document.getElementById("local-settings-strip-vertical").checked = document.body.classList.contains("msgr-vertical");
        document.getElementById("local-settings-strip-neighbors").checked = document.body.classList.contains("msgr-neighbors-only");
        document.getElementById("local-settings-audio-availability").checked = document.body.classList.contains("msgr-audio-availability");
        populate_microphone_device_list();

        if (panel.style.display === "none")
        {
            panel.classList.remove("closing");

            // clear it rather than force "block": an inline display beats the stylesheet,
            // and the phone themes need the panel to stay the flex column that lets the
            // row list scroll under a fixed title bar and save button
            panel.style.display = "";
        }
        else
        {
            close_local_settings();
        }
    };

    // fade OUT as well as in - display:none mid-animation just made it vanish.
    // a declaration, not a let: it is referenced above this line too, and hoisting
    // keeps that working if this block is ever reordered
    function close_local_settings()
    {
        let panel = document.getElementById("local-settings-container");
        panel.classList.add("closing");

        window.setTimeout(function()
        {
            panel.style.display = "none";
            panel.classList.remove("closing");
        }, 200);
    }

    // the select and the touch segments are two faces of g_is_continuous_mic_mode; a theme
    // shows one of them, and this paints whichever is on screen from that one variable
    function render_mic_mode_controls()
    {
        document.getElementById("local-settings-mic-mode").value = g_is_continuous_mic_mode ? "continuous" : "push-to-talk";
        document.getElementById("local-settings-ptt-key").value = g_push_to_talk_key_label;

        // the key only means anything in push-to-talk mode
        document.getElementById("local-settings-ptt-key-row").style.display = g_is_continuous_mic_mode ? "none" : "";

        let segments = document.querySelectorAll("#local-settings-mic-mode-segmented .local-settings-segment");

        for (let x = 0; x < segments.length; x++)
        {
            let is_selected = (segments[x].getAttribute("data-mic-mode") === "continuous") === g_is_continuous_mic_mode;
            segments[x].classList.toggle("segment-selected", is_selected);
        }
    }

    // fills the microphone picker with the audio inputs the browser reports. the row always
    // shows; before the microphone has been used once the browser hides the real device list
    // behind the permission, so a disabled hint entry explains why there is nothing to pick yet
    function populate_microphone_device_list()
    {
        let row = document.getElementById("local-settings-mic-device-row");

        if (row == null || navigator.mediaDevices == null || typeof navigator.mediaDevices.enumerateDevices != "function")
        {
            return;
        }

        navigator.mediaDevices.enumerateDevices().then(function(devices)
        {
            // a device with an empty id is the pre-permission placeholder, not a real choice
            let inputs = devices.filter(function(device) { return device.kind === "audioinput" && device.deviceId !== ""; });

            let select = document.getElementById("local-settings-mic-device");
            let html = "<option value=\"\">system default</option>";

            if (inputs.length == 0)
            {
                html += "<option value=\"\" disabled>(use the microphone once to list devices)</option>";
            }

            for (let i = 0; i < inputs.length; i++)
            {
                let label = (typeof inputs[i].label === "string" && inputs[i].label.length > 0) ? inputs[i].label : ("microphone " + (i + 1));
                html += "<option value=\"" + chat__sanitize_string(inputs[i].deviceId) + "\">" + chat__sanitize_string(label) + "</option>";
            }

            select.innerHTML = html;
            select.value = g_selected_microphone_device_id;

            // a remembered device that no longer exists falls back to the default entry
            if (select.value !== g_selected_microphone_device_id)
            {
                select.value = "";
            }

            row.style.display = "";
        }).catch(function() { row.style.display = "none"; });
    }

    // switches the live capture to the chosen device without touching the capture graph: a new
    // source joins the same gain node and the old track just stops. nothing is ever disconnected,
    // because a capture-edge disconnect once stalled chromium's whole mic delivery
    function apply_selected_microphone_device()
    {
        if (g_local_audio_stream == null || navigator.mediaDevices == null)
        {
            return; // the mic is not built yet; the choice applies when it activates
        }

        let switch_constraints = (g_selected_microphone_device_id != "")
            ? { audio: { deviceId: { ideal: g_selected_microphone_device_id } } }
            : { audio: true };

        navigator.mediaDevices.getUserMedia(switch_constraints).then(function(new_stream)
        {
            let old_track = g_local_audio_stream.getAudioTracks()[0];
            let new_track = new_stream.getAudioTracks()[0];

            // the new track inherits the transmit state, so a switch mid-talk keeps talking
            new_track.enabled = (old_track != null) ? old_track.enabled : false;

            let new_source = g_audio_context.createMediaStreamSource(new_stream);
            new_source.connect(g_audio_recorder_gain_node);

            if (old_track != null)
            {
                old_track.stop();
            }

            g_local_audio_stream = new_stream;
            g_audio_input = new_source;

            utils__custom_log("microphone switched to: " + (new_track.label || "default device"));
        }).catch(function(switch_error)
        {
            utils__custom_alert("could not switch the microphone: " + switch_error);
        });
    }

    function apply_mic_mode(is_continuous)
    {
        g_is_continuous_mic_mode = is_continuous;
        utils__storage_set("lemon_continuous_mic", g_is_continuous_mic_mode ? "1" : "0");

        // switching modes mid-transmission: stop cleanly, the button state must not lie
        if (g_is_continuous_mic_mode == false && g_is_continuous_transmission_active == true)
        {
            g_is_continuous_transmission_active = false;
            document.getElementById("microphone-push-to-talk-button-touch-device").classList.remove("mic-continuous-active");
            voice__process_stop_sending_audio();
        }

        render_mic_mode_controls();
    }

    // every setting applies live, so this is a "save" only in the sense of "done"
    document.getElementById("local-settings-close-button").onclick = close_local_settings;
    document.getElementById("local-settings-x-button").onclick = close_local_settings;

    // the people-strip preferences moved here from the toolbar; the toolbar buttons
    // still exist for the strip themes, so both drive the same body classes
    document.getElementById("local-settings-strip-vertical").onchange = function()
    {
        document.body.classList.toggle("msgr-vertical", this.checked);
        utils__storage_set("lemon_strip_vertical", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-strip-neighbors").onchange = function()
    {
        document.body.classList.toggle("msgr-neighbors-only", this.checked);
        utils__storage_set("lemon_strip_neighbors", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-audio-availability").onchange = function()
    {
        document.body.classList.toggle("msgr-audio-availability", this.checked);
        utils__storage_set("lemon_audio_availability", this.checked ? "1" : "0");
    };

    // avatar / account actions: opens the same menu the msgr "me" button opens
    document.getElementById("local-settings-avatar-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");

        if (local_row == null)
        {
            utils__custom_alert("connect first to change your avatar");
            return;
        }

        close_local_settings();

        let forwarded_event = { stopPropagation: function() {}, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, window.innerWidth - 190, 48, local_row, false);
    };

    document.getElementById("local-settings-show-log").onchange = function()
    {
        let log_button = document.getElementById("show-hide-log-button");
        log_button.style.display = this.checked ? "" : "none";

        if (this.checked == false)
        {
            document.getElementById("textarea-log").style.display = "none";
        }

        utils__storage_set("lemon_show_log_button", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-show-avatars").onchange = function()
    {
        g_show_message_avatars = this.checked;
        utils__storage_set("lemon_show_message_avatars", this.checked ? "1" : "0");
    };

    // changing the key size changes the identity itself, so it only takes effect on the next
    // keypair: the current connection keeps the key it already has
    document.getElementById("local-settings-rsa-key-bits").onchange = function()
    {
        let chosen_bits = parseInt(this.value);

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(chosen_bits) < 0) { return; }

        g_rsa_key_bits = chosen_bits;
        utils__storage_set("lemon_rsa_key_bits", String(chosen_bits));

        // a size the user picked by hand replaces whatever a server last asked for
        g_rsa_key_too_weak_prompted_for_bits = 0;

        utils__custom_alert("identity key size set to " + chosen_bits + " bits. it applies to the next identity you create - use the identity button to switch now");
    };

    document.getElementById("rsa-key-too-weak-no-button").onclick = keys__hide_rsa_key_too_weak_dialog;
    document.getElementById("close-button-rsa-key-too-weak").onclick = keys__hide_rsa_key_too_weak_dialog;

    document.getElementById("rsa-key-too-weak-yes-button").onclick = function()
    {
        let target_bits = parseInt(this.getAttribute("data-target-bits"));

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(target_bits) < 0) { return; }

        g_rsa_key_bits = target_bits;
        utils__storage_set("lemon_rsa_key_bits", String(target_bits));

        keys__hide_rsa_key_too_weak_dialog();
        utils__custom_alert("creating a " + target_bits + "-bit identity key, this can take a while ...");

        // keep the same passphrase where there is one: at the new size it derives a different
        // keypair, but it stays reproducible from what the user already saved
        connection__request_identity((typeof g_identity_string === "string" && g_identity_string.length >= 199) ? g_identity_string : null);
        connection__request_connect("button");
    };
    document.getElementById("local-settings-seen-indicator").onchange = function()
    {
        g_show_seen_indicator = this.checked;
        utils__storage_set("lemon_seen_indicator", this.checked ? "1" : "0");
        chat__render_seen_indicator();
    };

    document.getElementById("local-settings-auto-scroll").onchange = function()
    {
        g_auto_scroll_chat_to_end = this.checked;
        utils__storage_set("lemon_auto_scroll", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-send-seen").onchange = function()
    {
        g_send_seen_receipts = this.checked;
        utils__storage_set("lemon_send_seen", this.checked ? "1" : "0");
    };
    document.getElementById("local-settings-hide-mic").onchange = function()
    {
        g_hide_microphone_button = this.checked;
        utils__storage_set("lemon_hide_mic", this.checked ? "1" : "0");
        voice__update_microphone_button();
    };

    document.getElementById("local-settings-file-logging").onchange = function()
    {
        g_is_file_logging_enabled = this.checked;

        // java saves it and turns the logging on or off on both sides
        if (typeof Android !== "undefined" && typeof Android.JavaExportSetFileLogging === "function")
        {
            Android.JavaExportSetFileLogging(this.checked);
        }
    };

    document.getElementById("local-settings-sound-effects").onchange = function()
    {
        // route through the one toggle that also syncs the mute gate and the android side
        if (this.checked != g_are_sound_effects_enabled)
        {
            UI.sound_effects_button_onclick();
        }
        utils__storage_set("lemon_sound_effects", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-country-flags").onchange = function()
    {
        UI.hide_show_flags_button_onclick();
        utils__storage_set("lemon_show_flags", this.checked ? "1" : "0");
    };

    document.getElementById("local-settings-mic-mode").onchange = function()
    {
        apply_mic_mode(this.value === "continuous");
    };

    document.getElementById("local-settings-mic-device").onchange = function()
    {
        g_selected_microphone_device_id = this.value;
        utils__storage_set("lemon_mic_device_id", g_selected_microphone_device_id);
        apply_selected_microphone_device();
    };

    {
        let segments = document.querySelectorAll("#local-settings-mic-mode-segmented .local-settings-segment");

        for (let x = 0; x < segments.length; x++)
        {
            segments[x].onclick = function()
            {
                apply_mic_mode(this.getAttribute("data-mic-mode") === "continuous");
            };
        }
    }

    // press the button, then press the key you want. escape cancels
    document.getElementById("local-settings-ptt-key").onclick = function()
    {
        let key_button = this;
        key_button.value = "press a key…";

        let capture_key = function(event)
        {
            event.preventDefault();
            event.stopPropagation();
            window.removeEventListener("keydown", capture_key, true);

            if (event.key !== "Escape")
            {
                g_push_to_talk_key_code = event.which || event.keyCode;
                g_push_to_talk_key_label = (event.key.length === 1) ? event.key.toUpperCase() : event.key;

                try
                {
                    localStorage.setItem("lemon_ptt_key_code", g_push_to_talk_key_code);
                    localStorage.setItem("lemon_ptt_key_label", g_push_to_talk_key_label);
                }
                catch (e) { }
            }

            key_button.value = g_push_to_talk_key_label;
        };

        window.addEventListener("keydown", capture_key, true);
    };

    // these toggles live in local settings now; hiding the toolbar copies frees icon space
    document.getElementById("sound-effects-button").style.display = "none";
    document.getElementById("hide-show-flags-button").style.display = "none";
    document.getElementById("microphone-always-broadcasting-audio-button").style.display = "none";

    // restore the persisted sound preference; runs after the var initializers, so it wins
    let stored_sound = utils__storage_get("lemon_sound_effects");
    if (stored_sound != null)
    {
        g_are_sound_effects_enabled = (stored_sound === "1");
        sounds__apply_sound_effects_muted();
    }
    main__sync_toolbar_state_classes(); // seed the state classes from the initial bools so oldschool shows the right icons on load
    document.getElementById("musicbot-management-close-button").onclick = UI.musicbot_management_close_button_onclick;
    document.getElementById("musicbot-management-background-container-upload-button-confirm").onclick = UI.musicbot_management_confirm_upload_button_onclick;

    document.querySelector("a-toast").onclick = UI.a_toast_onclick;
    document.addEventListener("contextmenu", e =>
    {
        e.preventDefault();
    });

    if (g_is_client_running_under_touch_device)
    {
        document.getElementById("microphone-push-to-talk-button-touch-device").addEventListener("touchstart", UI.toggle_microphone_onclick);
        document.getElementById("microphone-push-to-talk-button-touch-device").addEventListener("touchend", UI.toggle_microphone_onclick);
    }
    else
    {
        document.getElementById("toggle-microphone").addEventListener("change", UI.toggle_microphone_onclick);
    }

    for (let i = 0; i < document.getElementsByClassName("server-settings-tab-li").length; i++)
    {
        document.getElementsByClassName("server-settings-tab-li")[i].onclick = UI.server_settings_tab_li_onclick;
    }

    // identity list: delete buttons are rendered dynamically, so delegate their clicks. a
    // delete sends the identity hash; the server clears it and strips the holder's tags
    // send an add/remove of a single tag on a stored identity (works offline)
    let send_modify_identity_tag = function(identity_hash, tag_id, add)
    {
        if (identity_hash == null || identity_hash.length == 0) { return; }
        let message_object = { message: { type: "modify_identity_tag", public_key_hash: identity_hash, tag_id: parseInt(tag_id), add: add } };
        connection__send_message_object(message_object);
    };

    // registers (or with an empty alias, unregisters) a STORED identity by hash, so it works
    // whether or not its owner is connected. an empty result is normal: the server refuses
    // silently when the alias is already taken by somebody else
    let send_set_identity_alias = function(identity_hash, alias)
    {
        if (identity_hash == null || identity_hash.length == 0) { return; }

        let message_object = { message: { type: "set_identity_alias_request", public_key_hash: identity_hash, alias: ("" + alias).trim() } };
        connection__send_message_object(message_object);

        // the list is a server-rendered snapshot; ask for a fresh one so the alias and the
        // registered column reflect what the server actually accepted
        setTimeout(function()
        {
            let refresh_object = { message: { type: "request_identity_list" } };
            let refresh_json = connection__process_message_before_sending(refresh_object);
            connection__websocket_worker_send(keys__encrypt_all_message_data_and_convert_to_base64(refresh_json));
        }, 250);
    };

    document.getElementById("server-settings-identities-list").addEventListener("click", function(event)
    {
        // remove a single tag from an identity (the chip's ✕)
        let tag_remove = event.target.closest(".identity-tag-remove");
        if (tag_remove != null)
        {
            send_modify_identity_tag(tag_remove.getAttribute("data-identity-hash"), tag_remove.getAttribute("data-tag-id"), false);
            return;
        }

        // save the alias typed into this row (registers the identity)
        let alias_set_button = event.target.closest(".identity-alias-set-button");
        if (alias_set_button != null)
        {
            let alias_input = document.querySelector('.identity-alias-input[data-identity-hash="' + alias_set_button.getAttribute("data-identity-hash") + '"]');
            send_set_identity_alias(alias_set_button.getAttribute("data-identity-hash"), (alias_input != null) ? alias_input.value : "");
            return;
        }

        // clear the alias (unregisters the identity)
        let alias_clear_button = event.target.closest(".identity-alias-clear-button");
        if (alias_clear_button != null)
        {
            send_set_identity_alias(alias_clear_button.getAttribute("data-identity-hash"), "");
            return;
        }

        // delete the whole identity (the row's ✕)
        let button = event.target.closest(".identity-delete-button");
        if (button == null) { return; }

        let identity_hash = button.getAttribute("data-identity-hash");
        if (identity_hash == null || identity_hash.length == 0) { return; }

        let message_object = { message: { type: "delete_identity", public_key_hash: identity_hash } };
        connection__send_message_object(message_object);
    });

    // enter in the alias field saves it, same as pressing the ✓
    document.getElementById("server-settings-identities-list").addEventListener("keyup", function(event)
    {
        if (event.key != "Enter") { return; }

        let alias_input = event.target.closest(".identity-alias-input");
        if (alias_input == null) { return; }

        send_set_identity_alias(alias_input.getAttribute("data-identity-hash"), alias_input.value);
    });

    // give an identity a tag (the "+ tag" dropdown)
    document.getElementById("server-settings-identities-list").addEventListener("change", function(event)
    {
        let select = event.target.closest(".identity-tag-add");
        if (select == null || select.value === "") { return; }
        send_modify_identity_tag(select.getAttribute("data-identity-hash"), select.value, true);
        select.value = "";
    });

    // make the server settings window draggable by its left nav column (tab clicks still work,
    // since a click without movement does not reposition the window)
    (function() {
        let settings_window = document.getElementById("server-settings-tab");
        let drag_handle = document.getElementById("server-settings-tab-subcontainer");
        let is_dragging = false;
        let drag_offset_x = 0;
        let drag_offset_y = 0;

        if (settings_window != null && drag_handle != null)
        {
            drag_handle.addEventListener("mousedown", function(e) {
                let rect = settings_window.getBoundingClientRect();
                is_dragging = true;
                drag_offset_x = e.clientX - rect.left;
                drag_offset_y = e.clientY - rect.top;
                e.preventDefault();
            });

            document.addEventListener("mousemove", function(e) {
                if (is_dragging == false)
                {
                    return;
                }
                settings_window.style.left = (e.clientX - drag_offset_x) + "px";
                settings_window.style.top = (e.clientY - drag_offset_y) + "px";
            });

            document.addEventListener("mouseup", function() {
                is_dragging = false;
            });
        }
    })();

    // every popup drags by its title bar; the drag pins it with an inline fixed position, and the pin
    // is cleared on every re-open so popups start centered (a pin taken while hidden was 0,0 forever)
    (function() {
        let dragging = null, ox = 0, oy = 0;
        let bars = document.querySelectorAll('[id="menu-bar-container"]');

        let clear_drag_pin = function(dialog) {
            dialog.style.position = "";
            dialog.style.margin = "";
            dialog.style.left = "";
            dialog.style.top = "";
        };

        // recenter on every open: watch each dialog's container for its display flipping on
        let recenter_observer = new MutationObserver(function(mutations) {
            for (let i = 0; i < mutations.length; i++) {
                let container = mutations[i].target;
                if (container.style.display == "none" || container.style.display == "") { continue; }
                let dialog = container.querySelector('[data-recenter-on-open="true"]');
                if (dialog != null)
                {
                    // center by js instead of trusting theme css: some themes (oldschool)
                    // give the overlay container zero size, which parked dialogs top-left
                    clear_drag_pin(dialog);
                    dialog.style.position = "fixed";
                    dialog.style.margin = "0";
                    // measure with offsetWidth/Height (layout size) not getBoundingClientRect:
                    // a theme's pop-in scale animation (e.g. termix) makes the client rect
                    // smaller than the real box and pushed the centering off. center on both axes.
                    let dw = dialog.offsetWidth;
                    let dh = dialog.offsetHeight;
                    dialog.style.left = Math.max(0, Math.round((window.innerWidth - dw) / 2)) + "px";
                    dialog.style.top = Math.max(0, Math.round((window.innerHeight - dh) / 2)) + "px";
                }
            }
        });

        for (let i = 0; i < bars.length; i++)
        {
            let dialog = bars[i].parentElement;
            dialog.setAttribute("data-recenter-on-open", "true");
            if (dialog.parentElement != null)
            {
                recenter_observer.observe(dialog.parentElement, { attributes: true, attributeFilter: ["style"] });
            }

            bars[i].addEventListener("mousedown", function(e) {
                if (e.target.closest(".close-button") != null) { return; }
                let dialog = this.parentElement;
                let rect = dialog.getBoundingClientRect();
                if (rect.width == 0 && rect.height == 0) { return; } // no layout (hidden): a pin here would stick the dialog to 0,0
                dragging = dialog;
                ox = e.clientX - rect.left;
                oy = e.clientY - rect.top;
                dialog.style.position = "fixed";
                dialog.style.margin = "0";
                dialog.style.left = rect.left + "px";
                dialog.style.top = rect.top + "px";
                e.preventDefault();
            });
        }
        document.addEventListener("mousemove", function(e) {
            if (dragging == null) { return; }
            dragging.style.left = (e.clientX - ox) + "px";
            dragging.style.top = (e.clientY - oy) + "px";
        });
        document.addEventListener("mouseup", function() { dragging = null; });
    })();

    for (let i = 0; i < document.getElementsByClassName("choose-theme-item").length; i++)
    {
        document.getElementsByClassName("choose-theme-item")[i].onclick = UI.choose_theme_item_onclick;
    }

    // on a touch device, hide every theme not tagged mobile-capable, so a phone can only pick
    // themes that are actually laid out for it (currently just the "mobile" theme)
    if (g_is_client_running_under_touch_device)
    {
        let theme_items = document.getElementsByClassName("choose-theme-item");
        for (let i = 0; i < theme_items.length; i++)
        {
            if (theme_items[i].getAttribute("data-mobile") !== "true")
            {
                theme_items[i].style.display = "none";
            }
        }
    }

    // browsers create an AudioContext suspended unless it is made during a user gesture; resume it on the first click/tap so audio works after an autoconnect, with no dedicated connect click.
    // the context that matters is audio_context (created at authentication time, all voice/music playback runs through it).
    // the listeners must stay attached until audio_context exists and is running: a gesture can fire before authentication creates it
    let unlock_audio_on_first_user_gesture = function()
    {
        if (typeof g_audio_context !== "undefined" && g_audio_context != null && g_audio_context.state === "suspended")
        {
            g_audio_context.resume();
        }

        if (typeof g_audio_context !== "undefined" && g_audio_context != null && g_audio_context.state === "running")
        {
            document.removeEventListener("click", unlock_audio_on_first_user_gesture, true);
            document.removeEventListener("touchend", unlock_audio_on_first_user_gesture, true);
            document.removeEventListener("keydown", unlock_audio_on_first_user_gesture, true);
        }
    };
    document.addEventListener("click", unlock_audio_on_first_user_gesture, true);
    document.addEventListener("touchend", unlock_audio_on_first_user_gesture, true);
    document.addEventListener("keydown", unlock_audio_on_first_user_gesture, true);

    g_data_processing_worker = audio_opus_glue__create_new_webworker_in_same_file("data_processing_worker");
    g_websocket_worker = audio_opus_glue__create_new_webworker_in_same_file("websocket_worker");
    g_opus_encoder_worker = audio_opus_glue__create_new_webworker_in_same_file("opus_encoder_worker");
    g_opus_decoder_worker = audio_opus_glue__create_new_webworker_in_same_file("opus_decoder_worker");
    g_minimp3_worker = audio_opus_glue__create_new_webworker_in_same_file("minimp3_worker");

    // restore a persisted identity: the 200-char passphrase deterministically recreates the keypair,
    // so the same identity survives relaunches. it is private-key-equivalent in localStorage, hence
    // server opt-in only (persist_identity in the served config); off keeps every window a fresh identity
    let persist_identity_enabled = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true);

    // avatars: server opt-in (default off) + the accepted max upload size (default 50 KB)
    g_server_policy.allow_avatars = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.allow_avatars === true);
    if (g_server_policy.allow_avatars == true && typeof window.__SERVER_CONFIG__.avatar_max_size === "number" && window.__SERVER_CONFIG__.avatar_max_size > 0)
    {
        g_server_policy.avatar_max_size = window.__SERVER_CONFIG__.avatar_max_size;
    }

    let persisted_identity = null;
    if (persist_identity_enabled == true)
    {
        persisted_identity = utils__storage_get("lemon_identity_string");
    }

    // only trust a stored value of the expected length; a short/corrupt one would silently seed
    // a DIFFERENT keypair, so fall back to a fresh random identity in that case
    let use_persisted_identity = (persisted_identity != null && persisted_identity.length >= 199);

    // the android webview talks to node, which owns the identity - no keypair needed there
    if (typeof Android === "undefined")
    {
        connection__request_identity(use_persisted_identity ? persisted_identity : null);
    }

    g_minimp3_worker.postMessage({
        type: "init"
    });

    // set css settings, set default theme
    // chat will support few themes at most, its up to people to modify their client.html to use own theme

    for (let i = 0; i < document.getElementsByClassName("style-theme-html-element").length; i++)
    {
        document.getElementsByClassName("style-theme-html-element")[i].setAttribute("media","max-width: 1px");
    }

    if (g_is_client_running_under_touch_device)
    {
        document.getElementById("style-theme-default-mobile").removeAttribute("media"); // set default theme
    }
    else
    {
        document.getElementById("style-theme-default").removeAttribute("media"); // set default theme
    }

    // server-baked default theme (from the injected config); applied unless the user has their own saved choice. persist=false so it never sticks.
    // on a phone a desktop theme is ignored here so the mobile default set above stays in effect
    let startup_theme_applied = false;

    if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.theme && UI.is_theme_allowed_on_this_device(window.__SERVER_CONFIG__.theme))
    {
        UI.apply_theme(window.__SERVER_CONFIG__.theme, false);
        startup_theme_applied = true;
    }

    // restore a theme the user previously picked (from the in-app menu); wins over the server default.
    // a saved desktop theme is ignored on a phone (keeps the mobile default)
    let saved_theme = utils__storage_get("lemon_theme");
    if (saved_theme != null && UI.is_theme_allowed_on_this_device(saved_theme))
    {
        UI.apply_theme(saved_theme, false);
        startup_theme_applied = true;
    }

    // no server-baked theme and nothing saved: activate the default sheet explicitly.
    // without this ALL four theme <style> elements stay active at once and the cascade
    // mixes them (the last sheet mostly wins), which is never an intended look
    if (startup_theme_applied == false)
    {
        UI.apply_theme(g_is_client_running_under_touch_device ? "default-mobile" : "default", false);
    }

    // the wrapper app's mode drives the theme (both directions). runs after the restore
    // above so it wins, and is re-run whenever android pushes a mode change
    main__apply_theme_for_app_mode();

    // restore the flat-channel-list preference (it only has a visible effect under a theme that
    // styles the .channels-flattened class, i.e. termix, but the class is applied regardless so
    // switching back to termix keeps the choice)
    g_is_channel_list_flattened = (utils__storage_get("lemon_channels_flat") == "1");
    UI.apply_channel_flatten_state();

    if (g_is_running_in_android_webview)
    {
        g_send_go_to_idle_mode_request = android_js_bridge.send_go_to_idle_mode_request_android;
        g_send_come_from_idle_mode_request = android_js_bridge.send_come_from_idle_mode_request_android;
        g_mark_call_accept_presence = android_js_bridge.mark_call_accept_presence_android;
        g_set_username_on_connect = android_js_bridge.set_username_on_connect_android;
        g_accept_current_settings_from_android = android_js_bridge.accept_current_settings_from_android;
        g_nudge_loopback_reattach = android_js_bridge.nudge_loopback_reattach_android;
        g_show_connection_phase = android_js_bridge.show_connection_phase_android;

        // a push that raced ahead of this wiring was held by the page shim - use it now
        if (typeof g_pending_android_settings === "string" && g_pending_android_settings != null)
        {
            console.log("connect-path: applying settings that arrived before onload");
            g_accept_current_settings_from_android(g_pending_android_settings);
            g_pending_android_settings = null;
        }

        // lets a theme style itself for the wrapper app (the strip themes give the settings gear a
        // real slot in its top bar; css alone cannot tell app from browser)
        document.body.classList.add("android-app");

        // the app loads client.html from its assets, so the served config (and allow_avatars) never
        // arrives; assume avatars here, a server that has them off just ignores the upload
        g_server_policy.allow_avatars = true;

        document.getElementById("import-identity-button").style.display = "none";

        // the bar gear stays hidden on the connect screen (the in-flow button below
        // owns that state) and appears once connected. tapped while connected it warns
        // first: the user is about to edit the details of the connection he is on
        document.getElementById("android-settings-button").onclick = function() {
            if (g_is_authenticated)
            {
                document.getElementById("android-settings-warning-container").style.display = "block";
                return;
            }
            Android.JavaExportGoToSettings();
        }

        document.getElementById("android-settings-warning-continue-button").onclick = function() {
            document.getElementById("android-settings-warning-container").style.display = "none";
            Android.JavaExportGoToSettings();
        }

        let close_settings_warning = function() {
            document.getElementById("android-settings-warning-container").style.display = "none";
        }
        document.getElementById("android-settings-warning-cancel-button").onclick = close_settings_warning;
        document.getElementById("close-button-android-settings-warning").onclick = close_settings_warning;

        // in-flow twin of the top-bar gear: on the not-connected screen the user
        // should not have to hunt a corner icon to find where the address lives
        document.getElementById("android-server-settings-container").style.display = "";
        document.getElementById("android-server-settings-button").onclick = function() {
            Android.JavaExportGoToSettings();
        }

        Android.JavaExportRequestCurrentSettingsFromAndroid();

        // the settings callback sets the connection target; the driver takes it from there
    }
    else if (g_are_server_details_predefined == true)
    {
        // website with baked-in config: connection__request_connect dials it under autoconnect
        connection__request_connect("settings");
    }

    // the page loads with the spinner up (see the template); arm its reveal deadline so a
    // client that connects to nothing still shows the connect page after a moment.
    // the setter call paints the theme's background onto it (opaque page one)
    connection__set_connect_holdback_loader_visible(true);
    g_connect_holdback_started = new Date().valueOf();
    g_connect_holdback_deadline = g_connect_holdback_started + 2500;
    setTimeout(connection__connect_holdback_check, 250);

    // a browser client in manual mode knows at load that nothing will dial: no spinner
    if (g_is_running_in_android_webview == false && g_is_autoconnect_without_user_action_active == false)
    {
        connection__reveal_connect_page();
    }

    connection__start_connection_status_ticker();
    connection__connection_driver();
}

// needs to be checked so webworkers that this code is shared with do not try to use window object

if (typeof window !== 'undefined')
{
    window.onload = main__window_onload;
}

// the export seam for the headless node build: every g_* and both message tables are locals of
// moduleFactory, and this object is what a node host gets from module.exports = factory();
// a page takes root.what = factory() and simply discards it
return {
    server_msg: server_msg,
    client_msg: client_msg,
    // the java -> js entry points (messages.js:3183). the service runtime needs these:
    // accept_current_settings_from_android is how the android settings screen reaches the client
    android_js_bridge: android_js_bridge,

    // call once before connecting: stands up the transport and the log sink
    init_node_runtime: android_host__init_node_runtime,

    // the webview handover: false closes the socket and parks reconnect, true re-arms
    node_set_connection_wanted: android_host__node_set_connection_wanted,

    /**
     * @brief sets the username this client asks for while connecting, because a headless host has no page variable to edit
     *
     * @param string username -> the name, "" goes back to the assigned one
     *
     * @return void
     */
    node_set_chosen_username: function(username)
    {
        g_chosen_username = (typeof username === "string") ? username : "";
    },

    /**
     * @brief a plaintext request from the loopback ui: encrypts it and sends it to the real server
     *
     * @param string json_string -> the request as json text
     *
     * @return void
     */
    node_forward_raw_request: function(json_string)
    {
        connection__websocket_worker_send(keys__encrypt_all_message_data_and_convert_to_base64(json_string));
    },

    /**
     * @brief registers the callback that gets every decrypted server frame raw; feeds the ui replay
     *
     * @param function callback -> callback(json_string), null clears it
     *
     * @return void
     */
    set_frame_listener: function(callback)
    {
        g_node_frame_listener = (typeof callback === "function") ? callback : null;
    },

    /**
     * @brief registers a callback for every connection status change; additive, every caller gets called
     *
     * @param function callback -> callback(status)
     *
     * @return void
     */
    set_connection_status_listener: function(callback)
    {
        if (typeof callback === "function") { g_connection_status_listeners.push(callback); }
    },

    /**
     * @brief registers the callback that gets the unread total whenever it changes; drives the app icon badge
     *
     * @param function callback -> callback(total), null clears it
     *
     * @return void
     */
    set_unread_listener: function(callback)
    {
        g_node_unread_listener = (typeof callback === "function") ? callback : null;
    },

    /**
     * @brief registers the callback for an incoming call: headless node's only route to the native accept/decline screen
     *
     * @param function callback -> callback(caller_username, channel_id), null clears it
     *
     * @return void
     */
    set_incoming_call_listener: function(callback)
    {
        g_node_incoming_call_listener = (typeof callback === "function") ? callback : null;
    },

    /**
     * @brief a ui attached to or left the loopback
     *        while one is attached the user is looking, so node stops counting unread messages and
     *        clears what it accumulated; the ui owns the badge then
     *
     * @param boolean is_attached -> true when a ui is attached
     * @param boolean is_from_host -> true when java said so; the webview redialling in the background is not the user coming back
     *
     * @return void
     */
    node_set_ui_attached: function(is_attached, is_from_host)
    {
        if (is_from_host === true)
        {
            node_ui_visibility_from_host = true;
        }
        else if (node_ui_visibility_from_host == true)
        {
            // the webview redialling the loopback in the background is not the user
            // coming back, and java has already said so
            return;
        }

        let was_attached = g_node_has_attached_ui;
        g_node_has_attached_ui = (is_attached === true);

        if (g_node_has_attached_ui == true)
        {
            g_channel_unread_counts = {};

            for (let i = 0; i < g_client_list.length; i++)
            {
                g_client_list[i].unread_count = 0;
            }

            chat__update_app_unread_badge();
        }

        // headless node is a client nobody is sitting at, so it belongs in idle -
        // that is what lets other people CALL this phone. without this it just stood
        // in the root channel looking present
        if (g_is_authenticated == true && was_attached != g_node_has_attached_ui)
        {
            android_host__node_apply_idle_for_ui_state();
        }
    },

    /**
     * @brief java's connectivity watch, the only trustworthy "is there a network" on android
     *
     * @param boolean is_available -> true while wifi or mobile data is on
     *
     * @return void
     */
    node_set_device_network: function(is_available)
    {
        g_device_has_network = (is_available === true);

        // say it immediately - the retry text would otherwise keep the stale guess
        if (g_device_has_network === false && g_is_authenticated == false)
        {
            g_last_disconnect_reason = "no network connection (wifi and mobile data are off)";
            connection__report_connection_status(g_connection_status.state, g_last_disconnect_reason);
        }
    },

    /**
     * @brief the current connection status
     *
     * @return object g_connection_status, the state and its reason
     */
    get_connection_status: function()
    {
        return g_connection_status;
    },

    /**
     * @brief a ui attached to the loopback wants to be online; an attach is always the downstream of an authorized decision, so the driver dials for it
     *
     * @return void
     */
    node_connect_intent: function()
    {
        if (g_have_received_android_settings == true)
        {
            connection__request_connect("attach");
        }
    },

    /**
     * @brief the cached authentication frame, whenever it arrived; a replay leads with it regardless of start order
     *
     * @return string|null the frame, null before authentication
     */
    get_auth_frame: function()
    {
        return g_node_cached_auth_frame;
    },

    /**
     * @brief registers a callback fired after every dispatched message, the host's "state may have changed" signal; additive, every caller gets called
     *
     * @param function callback -> callback(message_type, had_error)
     *
     * @return void
     */
    set_on_message_processed: function(callback)
    {
        if (typeof callback === "function") { g_node_message_listeners.push(callback); }
    },

    /**
     * @brief a snapshot of the shared state for the host, built fresh per call since several entries are reassigned rather than mutated
     *        read-only: the arrays come back live, and writing through them bypasses the handlers' invariants
     *
     * @return object the client and channel lists, chat contexts, tags, icons, the current channel and its keys, the local ids and the connection flags
     */
    read_state: function()
    {
        return {
            g_client_list: g_client_list,
            g_channel_list: g_channel_list,
            g_chat_context_array: g_chat_context_array,
            g_map_client_id_to_array_index: g_map_client_id_to_array_index,
            g_offline_client_list: g_offline_client_list,
            g_chat_message_author_public_keys: g_chat_message_author_public_keys,
            g_tags: g_tags,
            g_icons: g_icons,
            current_channel_id: g_current_channel_id,
            current_channel_keys: g_current_channel_keys,
            local_client_id: g_local_client_id,
            g_local_username: g_local_username,
            g_current_chat_context_id: g_current_chat_context_id,
            g_chat_message_receiver_type: g_chat_message_receiver_type,
            g_is_client_list_retrieved: g_is_client_list_retrieved,
            g_is_channel_list_retrieved: g_is_channel_list_retrieved,
            // the live connection signal (g_is_websocket_connected is a dead flag, see ui.js)
            g_is_authenticated: g_is_authenticated,
            g_is_microphone_always_on: g_is_microphone_always_on,
            g_have_received_android_settings: g_have_received_android_settings
        };
    }
};
}));

