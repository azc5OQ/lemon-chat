/**
 * @brief uses the device's server default unless the user has saved an allowed theme
 */
function main__apply_startup_theme()
{
    let config = window.__SERVER_CONFIG__ || {};
    let theme = g_is_client_running_under_touch_device ? "default-mobile" : "default";
    let server_theme = g_is_client_running_under_touch_device ? (config.mobile_theme || config.theme) : config.theme;
    let saved_theme = utils__storage_get("lemon_theme");

    // older pages may supply one default. mobile still requires a mobile theme
    if (server_theme && document.getElementById("style-theme-" + server_theme) && UI.is_theme_allowed_on_this_device(server_theme))
    {
        theme = server_theme;
    }

    // a user's own choice takes priority over the server's suggestion
    if (saved_theme && document.getElementById("style-theme-" + saved_theme) && UI.is_theme_allowed_on_this_device(saved_theme))
    {
        theme = saved_theme;
    }
    UI.apply_theme(theme, false);
}

// starts the browser client and exports the Node API
// keep this include last to close the aes-js.js factory

// state private to this file
// native visibility overrides loopback attachment once Java reports it
var node_ui_visibility_from_host = false;
var main_is_push_to_talk_key_down = false;

/**
 * @brief closes context menus when the pointer is pressed outside them
 *
 * @param object event -> the mouse event
 *
 * @return void
 */
function main__document_onmousedown(event)
{
    UI.delete_chat_message_contextmenu();

    let is_channel_list_contextmenu_delete_needed = !event.target.closest(".context-menu-item, .channel-list-context-menu, .chat-input-container-context-menu");

    if (is_channel_list_contextmenu_delete_needed)
    {
        UI.delete_contextmenus(true);
    }
}

/**
 * @brief checks whether the focused element accepts text input
 *
 * @param object element -> the focused DOM element
 *
 * @return boolean true when the element accepts editable text
 */
function main__is_editable_field(element)
{
    if (element == null)
    {
        return false;
    }
    if (element.isContentEditable)
    {
        return true;
    }
    if (element.readOnly || element.disabled)
    {
        return false;
    }
    return element.tagName === "TEXTAREA" || (element.tagName === "INPUT" && ["text", "search", "email", "url", "password", "tel", "number"].includes(element.type));
}

/**
 * @brief starts speech while the configured talk key is held
 *
 * @param object event -> the keyboard event
 *
 * @return void
 */
function main__document_onkeydown(event)
{
    let focused_element = document.activeElement;

    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    if (event.which == g_push_to_talk_key_code)
    {
        if (g_is_push_to_talk_disabled_while_typing == true && main__is_editable_field(focused_element))
        {
            return;
        }
        main_is_push_to_talk_key_down = true;
        voice__process_start_sending_audio();
    }
}

/**
 * @brief stops keyboard speech when the talk key is released
 *
 * @param object event -> the keyboard event
 *
 * @return void
 */
function main__document_onkeyup(event)
{
    if (event.which != g_push_to_talk_key_code || main_is_push_to_talk_key_down == false)
    {
        return;
    }
    main_is_push_to_talk_key_down = false;
    if (g_is_microphone_always_on == true || g_is_continuous_mic_mode == true)
    {
        return;
    }
    // finish the last audio frame before stopping after a normal key release
    voice__process_stop_sending_audio();
}

/**
 * @brief stops keyboard speech when focus moves to a text field
 *
 * @return void
 */
function main__chat_input_onfocus()
{
    if (g_is_push_to_talk_disabled_while_typing == true && main_is_push_to_talk_key_down == true && g_is_microphone_always_on == false && g_is_continuous_mic_mode == false)
    {
        main_is_push_to_talk_key_down = false;
        voice__process_stop_sending_audio_now();
    }
}

/**
 * @brief applies the theme required by the Android app mode
 *
 * @return void
 */
function main__apply_theme_for_app_mode()
{
    // simple mode starts with Bluebell; a theme the user chose in the theme list is kept
    if (g_android_app_mode == "simple")
    {
        let chosen_theme = utils__storage_get("lemon_theme");
        UI.apply_theme(chosen_theme != null && UI.is_theme_allowed_on_this_device(chosen_theme) ? chosen_theme : "bluebell", false);
        return;
    }

    // advanced mode keeps the saved theme or uses default-mobile
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
 * @brief stops an event from reaching parent handlers
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
 * @brief updates toolbar classes for sound, microphone and chat visibility
 *
 * @return void
 */
function main__sync_toolbar_state_classes()
{
    let sfx = document.getElementById("sound-effects-button");
    let mic = document.getElementById("microphone-always-broadcasting-audio-button");
    let hide = document.getElementById("hide-chat-button");
    if (sfx != null)
    {
        sfx.classList.toggle("sfx-on", g_are_sound_effects_enabled == true);
    }
    if (mic != null)
    {
        mic.classList.toggle("mic-on", g_is_microphone_always_on == true);
    }
    if (hide != null)
    {
        hide.classList.toggle("chat-hidden", g_is_chat_hidden == true);
    }
}

/**
 * @brief checks whether the current theme is marked for mobile use
 *
 * @return boolean true while a theme for phones is in use
 */
function main__is_current_theme_for_phones()
{
    let item = document.querySelector('.choose-theme-item[data-name="' + connection__current_theme_name() + '"]');

    return item != null && item.getAttribute("data-mobile") === "true";
}

/**
 * @brief toggles the member list panel
 *
 * @param object event -> the click event of the button
 *
 * @return void
 */
function main__show_hide_member_list_button_onclick(event)
{
    event.currentTarget.classList.toggle("member-list-shown");
}

/**
 * @brief closes the member panel when its page overlay is pressed
 *
 * @param object event -> the click event of the page element (#communication-system-container)
 *
 * @return void
 */
function main__member_list_blurred_page_onclick(event)
{
    // only the blurred layer
    if (event.target !== event.currentTarget)
    {
        return;
    }

    // hide the list
    document.getElementById("show-hide-member-list-button").classList.remove("member-list-shown");
}

/**
 * @brief updates the conversation dropdown from the open tabs and selected chat
 *
 * @return void
 */
function main__refresh_chat_context_select()
{
    let select = document.getElementById("chat-context-select");
    let shown = Array.from(document.querySelectorAll("#chat-context-container > .chat-context")).find(function(context)
    {
        return getComputedStyle(context).display != "none";
    });
    let shown_value = (shown == null) ? "" : shown.id.replace("chat-context-channel-", "channel-").replace("chat-context-pm-", "user-");
    let option = null;

    // rebuild the entries
    select.replaceChildren();
    for (let tab of document.querySelectorAll("#chat-context-selectors-container .chat-context-selector"))
    {
        option = document.createElement("option");
        option.value = tab.getAttribute("data-chat-context-selector-id");
        option.textContent = (tab.getAttribute("data-chat-context-selector-type") == "channel" ? "# " : "@ ") + tab.querySelector("p").textContent.trim();
        option.selected = option.value == shown_value;
        select.appendChild(option);
    }

    // the own channel stays
    document.getElementById("chat-context-select-close").disabled = (shown_value == "" || shown_value == "channel-" + g_current_channel_id);

    let strip = document.getElementById("chat-context-selectors-container");
    if (strip.clientWidth > 0 && strip.dataset.revealedContext !== shown_value && getComputedStyle(strip).overflowX === "auto")
    {
        strip.dataset.revealedContext = shown_value;
        let active = Array.from(strip.children).find(function(tab)
        {
            return tab.getAttribute("data-chat-context-selector-id") === shown_value;
        });
        if (active != null)
        {
            let bounds = strip.getBoundingClientRect();
            let selected = active.getBoundingClientRect();
            if (selected.left < bounds.left)
            {
                strip.scrollLeft -= bounds.left - selected.left;
            }
            else if (selected.right > bounds.right)
            {
                strip.scrollLeft += selected.right - bounds.right;
            }
        }
    }
}

/**
 * @brief opens the conversation selected in the dropdown through its tab
 *
 * @param object event -> the change event of #chat-context-select
 *
 * @return void
 */
function main__chat_context_select_onchange(event)
{
    let tab = document.querySelector('#chat-context-selectors-container .chat-context-selector[data-chat-context-selector-id="' + event.currentTarget.value + '"]');

    // show the conversation
    if (tab != null)
    {
        tab.click();
    }
}

/**
 * @brief closes the selected conversation through its existing tab control
 *
 * @return void
 */
function main__chat_context_select_close_onclick()
{
    let mark = document.querySelector('#chat-context-selectors-container [data-chat-context-remove-selector-id="' + document.getElementById("chat-context-select").value + '"]');

    // close the conversation
    if (mark != null)
    {
        mark.click();
    }
}

// last page touch in milliseconds, or zero before the first touch
let main__last_touch_time = 0;

/**
 * @brief records touch timing to identify subsequent synthetic mouse events
 *
 * @return void
 */
function main__note_touch()
{
    main__last_touch_time = Date.now();
}

/**
 * @brief identifies mouse events arriving within one second of a touch
 *
 * @return boolean true for a mouse event that belongs to a touch
 */
function main__is_mouse_event_of_a_touch()
{
    return Date.now() - main__last_touch_time < 1000;
}

/**
 * @brief routes mouse actions to channel tree handlers on touch capable devices
 *
 * @param object event -> the mousedown, click or dblclick event of #channel-list-container or #idle-clients-container
 *
 * @return void
 */
function main__tree_mouse_on_touch_device(event)
{
    let client = event.target.closest(".connected-client");
    let channel = event.target.closest(".single-channel");
    let channel_name = event.target.closest("[data-channel-name-id]");
    // use the explicit row when currentTarget is absent
    let forwarded_event = { stopPropagation: function()
    {
    }, currentTarget: null, which: event.which, clientX: event.clientX, clientY: event.clientY };

    // only a real mouse
    if (g_is_client_running_under_touch_device == false || main__is_mouse_event_of_a_touch() == true)
    {
        return;
    }

    // a person's row
    if (client != null)
    {
        if (event.type == "mousedown")
        {
            UI.connected_user_onmousedown(forwarded_event, null, null, null, client);
            event.stopPropagation();
        }
        return;
    }

    // not a channel row
    if (channel == null || event.target.closest(".single-channel-collapse-button") != null)
    {
        return;
    }

    // a channel row
    if (event.type == "mousedown")
    {
        UI.single_channel_onmousedown(forwarded_event, null, null, null, channel);
        event.stopPropagation();
    }
    else if (channel_name != null && event.type == "dblclick")
    {
        UI.single_channel_doubleclick_join(event);
    }
    else if (channel_name != null)
    {
        UI.single_channel_onclick(event);
    }
}

/**
 * @brief applies the local setting for a custom theme cursor
 *
 * @return void
 */
function main__apply_custom_cursor_setting()
{
    let root = document.documentElement;

    if (root != null && root.classList != null)
    {
        root.classList.toggle("custom-cursor-off", g_is_custom_cursor_enabled == false);
    }
}

/**
 * @brief initializes the client, theme, connection settings and page controls
 *
 * @return promise ignored by window.onload. resolves once the page's handlers are attached
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

    // use platform detection with the Android WebView override
    if (utils__detect_touch_device() == true)
    {
        g_is_client_running_under_touch_device = true;
    }

    if (g_is_running_in_android_webview == true)
    {
        // Android provides its own connection settings screen
        document.getElementById("add-key-button").style.visibility = "hidden";
        document.getElementById("connect-form-grid").style.visibility = "hidden";
    }

    // apply served connection details before hiding the form. keep connection status visible
    if (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.port && g_is_running_in_android_webview == false)
    {
        g_are_server_details_predefined = true;
        g_is_autoconnect_without_user_action_active = true;
        g_autoconnect_details = {
            // served pages use their origin. local files use the configured address
            host: (typeof window.__SERVER_CONFIG__.host === "string" && window.__SERVER_CONFIG__.host.length > 0) ? window.__SERVER_CONFIG__.host : window.location.hostname,
            port: window.__SERVER_CONFIG__.port,
            keys: window.__SERVER_CONFIG__.keys,
            wss_port: window.__SERVER_CONFIG__.wss_port
        };
        document.getElementById("connect-form-grid").style.display = "none";
    }

    // the optional start page waits for its Connect button
    if (typeof start_page__init === "function")
    {
        start_page__init();
    }

    if (g_is_running_in_android_webview == false)
    {
        // predefined connection details bypass bookmarks and hide their controls
        if (g_are_server_details_predefined == true)
        {
            document.getElementById("server-bookmarks-container").style.display = "none";
        }

        if (g_are_server_details_predefined == false)
        {
            document.getElementById("connect-form-grid").style.visibility = "visible";
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

    // delegate member gestures to live tree rows because the displayed clones are replaced on refresh

    /**
     * @brief opens the channel chooser at the pointer and keeps it inside the viewport
     *
     * @param number click_x -> horizontal pointer position
     * @param number click_y -> vertical pointer position
     *
     * @return void
     */
    function member_list_show_channel_switch_menu(click_x, click_y)
    {
        UI.delete_contextmenus();

        // label the channel switch list
        let menu_items_html = "<p class='context-menu-item context-menu-item-disabled'>Switch to channel:</p>\n";
        for (let i = 0; i < g_channel_list.length; i++)
        {
            let channel = g_channel_list[i];
            let lock_html = (channel.is_using_password == true) ? "🔒 " : "";
            let current_html_class = (parseInt(channel.channel_id) == parseInt(g_current_channel_id)) ? " context-menu-item-disabled" : "";
            // keep the menu open until the item's click handler runs
            menu_items_html += "<p role='menuitem' tabindex='0' class='context-menu-item channel-switch-menu-item" + current_html_class + "' data-switch-channel-id='" + channel.channel_id + "'>" + lock_html + chat__sanitize_string(channel.name) + "</p>\n";
        }

        let menu_html = "<div class=\"channel-list-context-menu channel-switch-menu\" style=\"top: " + click_y + "px; left: " + click_x + "px;\">\n\
                            <div class='channel-list-context-menu-background'>\n\
                            </div>\n\
                            <div class='channel-list-context-menu-items'>\n\
                                " + menu_items_html + "\n\
                            </div>\n\
                        </div>";

        document.getElementById("contextmenus-container").insertAdjacentHTML("beforeend", menu_html);

        let menu = document.querySelector("#contextmenus-container .channel-list-context-menu:last-child");
        if (menu != null)
        {
            let menu_rect = menu.getBoundingClientRect();
            menu.setAttribute("role", "menu");
            menu.setAttribute("aria-label", "Choose channel");
            menu.style.left = utils__position_px(Math.max(8, Math.min(click_x, utils__viewport_width() - menu_rect.width - 8)));
            menu.style.top = utils__position_px(Math.max(8, Math.min(click_y, utils__viewport_height() - menu_rect.height - 8)));
        }

        let switch_items = document.getElementsByClassName("channel-switch-menu-item");
        for (let i = 0; i < switch_items.length; i++)
        {
            switch_items[i].onkeydown = function(event)
            {
                if (["Enter", " ", "ArrowDown", "ArrowUp", "Escape"].includes(event.key) == false)
                {
                    return;
                }
                event.preventDefault();
                event.stopPropagation();
                if (event.key == "Enter" || event.key == " ")
                {
                    this.click();
                }
                else if (event.key == "Escape")
                {
                    UI.delete_contextmenus(true);
                    let chooser = document.querySelector(".member-conversations-channel-switch") || document.getElementById("pocket-channel-switch");
                    if (chooser != null)
                    {
                        chooser.focus();
                    }
                }
                else
                {
                    let next = (i + (event.key == "ArrowDown" ? 1 : -1) + switch_items.length) % switch_items.length;
                    switch_items[next].focus();
                }
            };
            switch_items[i].onclick = function()
            {
                let switch_channel_id = this.getAttribute("data-switch-channel-id");
                UI.delete_contextmenus(true);

                // join chosen channel
                member_conversations__join_channel(switch_channel_id);
            };
        }
    }

    /**
     * @brief closes the channel chooser when pressed outside its items
     *
     * @param Event event -> pointer, mouse or touch press
     *
     * @return void
     */
    function member_list_dismiss_channel_switch_menu(event)
    {
        let items = document.querySelector(".channel-switch-menu .channel-list-context-menu-items");
        if (items != null && !items.contains(event.target))
        {
            UI.delete_contextmenus(true);
        }
    }

    // capture runs before the opening gesture creates the chooser
    for (let event_name of ("PointerEvent" in window ? ["pointerdown"] : ["mousedown", "touchstart"]))
    {
        document.addEventListener(event_name, member_list_dismiss_channel_switch_menu, { capture: true, passive: true });
    }

    // return true when handled so page cleanup does not close the new menu
    /**
     * @brief routes a member tile tap or hold to its channel, live user or saved contact
     *
     * @param Element target_element -> element pressed inside the member list
     * @param number click_x -> horizontal pointer position for menus
     * @param number click_y -> vertical pointer position for menus
     * @param boolean is_short_click -> true for a tap, false for a hold
     *
     * @return boolean -> true when a matching tile handled the press
     */
    function member_list_forward(target_element, click_x, click_y, is_short_click)
    {
        let channel_circle = target_element.closest(".member-list-channel");
        if (channel_circle != null)
        {
            if (is_short_click == true && !target_element.closest(".member-conversations-channel-switch"))
            {
                // show the channel conversation through its tab
                member_conversations__open_channel_chat(channel_circle.getAttribute("data-member-list-channel-id"));
            }
            // a hold opens the list of channels
            else
            {
                member_list_show_channel_switch_menu(click_x, click_y);
                let first = document.querySelector(".channel-switch-menu-item");
                if (first != null)
                {
                    first.focus();
                }
            }
            return true;
        }

        let clone = target_element.closest(".member-list-client");
        if (clone == null)
        {
            return false;
        }

        if (!is_short_click && !UI.are_user_context_menus_enabled())
        {
            UI.delete_contextmenus(true);
            return true;
        }

        if (is_short_click && clone.classList.contains("music-bot-client"))
        {
            return true;
        }

        // offline contacts have no live row. open their conversation or details directly
        if (clone.classList.contains("offline-contact") == true)
        {
            let offline_identity_hash = clone.dataset.identityHash;
            let offline_contact = channel_tree__get_stored_client_by_identity_hash(offline_identity_hash);

            // tap opens the conversation. hold shows the saved details
            if (is_short_click == true && offline_contact != null)
            {
                UI.open_offline_chat_context(offline_contact);
                if (typeof member_conversations__show_selected_chat === "function")
                {
                    member_conversations__show_selected_chat();
                }
            }
            else
            {
                UI.show_offline_contact_info_popup(offline_identity_hash);
            }
            return true;
        }

        let strip_client_id = clone.getAttribute("data-connected-client-id");
        if (strip_client_id == null)
        {
            return false;
        }

        let tree_row = document.querySelector('#channel-list-container .connected-client[data-connected-client-id="' + strip_client_id + '"], #idle-clients-container .connected-client[data-connected-client-id="' + strip_client_id + '"]');
        if (tree_row == null)
        {
            return false;
        }

        // pass the live tree row instead of its member list clone
        let forwarded_event = { stopPropagation: function()
        {
        }, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, click_x, click_y, tree_row, is_short_click);
        if (is_short_click && typeof member_conversations__show_selected_chat === "function" && g_current_chat_context_id === "chat-context-pm-" + strip_client_id)
        {
            member_conversations__show_selected_chat();
        }
        return true;
    }

    let member_list_body = document.getElementById("member-list-body");
    if (member_list_body != null)
    {
        let member_list_press_timer = null;
        let member_list_long_press = false;
        let member_list_moved = false;
        let member_list_press_target = null;
        let member_list_press_element = null;
        let member_list_press_generation = null;
        let member_list_press_channel = null;

        /**
         * @brief cancels the pending member tile hold and removes its touch listeners
         *
         * @return void
         */
        function cancel_member_list_press()
        {
            if (member_list_press_element != null)
            {
                member_list_press_element.removeEventListener("touchmove", move_member_list_press);
                member_list_press_element.removeEventListener("touchend", end_member_list_press);
                member_list_press_element.removeEventListener("touchcancel", cancel_member_list_press);
                member_list_press_element = null;
            }
            clearTimeout(member_list_press_timer);
            member_list_moved = true;
            member_list_long_press = false;
            member_list_press_target = null;
        }

        /**
         * @brief finds the pressed row again after a participant update replaces it
         *
         * @return Element the current row or null when the connection or channel changed
         */
        function current_member_list_press_target()
        {
            // use the authenticated session or connection phase
            if (member_list_press_target == null || !g_is_authenticated || g_worker_connection_ended || member_list_press_generation != g_worker_connection_generation || member_list_press_channel != g_current_channel_id)
            {
                return null;
            }
            for (let row of member_list_body.children)
            {
                if (row.getAttribute(member_list_press_target.attribute) == member_list_press_target.value)
                {
                    return member_list_press_target.channel_switch ? row.querySelector(".member-conversations-channel-switch") || row : row;
                }
            }
            return null;
        }

        /**
         * @brief cancels the pending menu when the finger moves
         */
        function move_member_list_press()
        {
            member_list_moved = true;
            clearTimeout(member_list_press_timer);
        }

        /**
         * @brief opens the selected conversation or completes a held menu
         *
         * @param TouchEvent event the completed touch
         */
        function end_member_list_press(event)
        {
            let target_element = current_member_list_press_target();
            let is_long_press = member_list_long_press;
            let is_short_press = !member_list_moved && target_element != null;
            cancel_member_list_press();
            if (is_long_press)
            {
                event.preventDefault();
            }
            else if (is_short_press)
            {
                // stops the browser from sending mouse events for the same tap after the layout changed
                event.preventDefault();
                member_list_forward(target_element, event.changedTouches[0].clientX, event.changedTouches[0].clientY, true);
            }
        }

        member_list_body.addEventListener("keydown", function(event)
        {
            if (["Enter", " ", "ContextMenu"].includes(event.key) == false && !(event.shiftKey && event.key == "F10"))
            {
                return;
            }
            let row = event.target.closest(".member-list-channel, .member-list-client");
            if (row == null)
            {
                return;
            }
            event.preventDefault();
            event.stopPropagation();
            let rect = row.getBoundingClientRect();
            member_list_forward(event.target, rect.left, rect.bottom, event.key == "Enter" || event.key == " ");
        });

        member_list_body.addEventListener("click", function(event)
        {
            // assistive technology can activate a button without preceding mouse or key events
            if (event.detail === 0 && event.target.closest(".member-conversations-channel-switch, .member-list-channel-name[role='button']"))
            {
                let rect = event.target.getBoundingClientRect();
                member_list_forward(event.target, rect.left, rect.bottom, true);
                event.stopPropagation();
            }
        });

        member_list_body.addEventListener("touchstart", function(event)
        {
            if (g_is_client_running_under_touch_device == false)
            {
                return;
            }

            cancel_member_list_press();
            if (event.touches.length != 1)
            {
                return;
            }
            let row = event.target.closest(".member-list-channel, .member-list-client");
            if (row == null)
            {
                return;
            }
            let attribute = row.hasAttribute("data-member-list-channel-id") ? "data-member-list-channel-id" : row.hasAttribute("data-identity-hash") ? "data-identity-hash" : "data-connected-client-id";
            member_list_press_target =
            {
                attribute: attribute,
                value: row.getAttribute(attribute),
                channel_switch: event.target.closest(".member-conversations-channel-switch") != null
            };
            member_list_press_generation = g_worker_connection_generation;
            member_list_press_channel = g_current_channel_id;
            member_list_press_element = event.target;
            member_list_press_element.addEventListener("touchmove", move_member_list_press, { passive: true });
            member_list_press_element.addEventListener("touchend", end_member_list_press, { passive: false });
            member_list_press_element.addEventListener("touchcancel", cancel_member_list_press);

            member_list_long_press = false;
            member_list_moved = false;

            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            member_list_press_timer = window.setTimeout(function()
            {
                let target_element = current_member_list_press_target();
                if (target_element == null)
                {
                    return;
                }
                member_list_long_press = true;
                member_list_forward(target_element, click_x, click_y, false);
            }, 600);
        }, { passive: true });

        member_list_body.addEventListener("scroll", cancel_member_list_press, { passive: true });
        document.getElementById("member-list-container").addEventListener("scroll", cancel_member_list_press, { passive: true });
        window.addEventListener("blur", cancel_member_list_press);
        document.addEventListener("visibilitychange", cancel_member_list_press);

        member_list_body.addEventListener("mousedown", function(event) {
            // skip after a touch
            if (event.which == 1 && g_is_client_running_under_touch_device == true && main__is_mouse_event_of_a_touch() == true)
            {
                return;
            }

            // left click opens the chat. right click opens its menu
            if (event.which == 1 || event.which == 3)
            {
                if (member_list_forward(event.target, event.clientX, event.clientY, event.which == 1) == true)
                {
                    // keep the focus given to the opened menu instead of returning it to the row
                    event.preventDefault();
                    event.stopPropagation();
                }
            }
        });
    }

    // offer channel switching on hold when the theme hides the tree
    let conversation_tab_container = document.getElementById("chat-context-selectors-container");
    if (conversation_tab_container != null)
    {
        /**
         * @brief checks whether the theme hides the channel tree pane
         *
         * @return boolean -> true when the pane exists and has no display
         */
        function is_channel_tree_hidden()
        {
            let tree_pane = document.getElementById("space-devider1");
            return (tree_pane != null && getComputedStyle(tree_pane).display == "none");
        }

        /**
         * @brief finds the channel tab containing a pressed element
         *
         * @param Element event_target -> element receiving the press
         *
         * @return Element|null -> enclosing channel tab or null
         */
        function tab_press_target(event_target)
        {
            let pill = event_target.closest('.chat-context-selector[data-chat-context-selector-type="channel"]');
            return pill;
        }

        let tab_press_timer = null;
        let tab_long_press = false;

        conversation_tab_container.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false)
            {
                return;
            }
            if (tab_press_target(event.target) == null || is_channel_tree_hidden() == false)
            {
                return;
            }

            tab_long_press = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            tab_press_timer = window.setTimeout(function() {
                tab_long_press = true;
                member_list_show_channel_switch_menu(click_x, click_y);
            }, 600);
        }, { passive: true });

        conversation_tab_container.addEventListener("touchmove", function() {
            clearTimeout(tab_press_timer);
        }, { passive: true });

        conversation_tab_container.addEventListener("touchend", function(event) {
            if (g_is_client_running_under_touch_device == false)
            {
                return;
            }

            clearTimeout(tab_press_timer);

            // a long press must not ALSO run the tab's own click
            if (tab_long_press == true)
            {
                event.preventDefault();
            }
        });

        conversation_tab_container.addEventListener("mousedown", function(event) {
            if (g_is_client_running_under_touch_device == true)
            {
                return;
            }

            if (event.which == 3 && tab_press_target(event.target) != null && is_channel_tree_hidden() == true)
            {
                event.stopPropagation(); // or main__document_onmousedown closes it in the same event
                member_list_show_channel_switch_menu(event.clientX, event.clientY);
            }
        });
    }

    // a stationary message touch opens the same menu as a right click
    let chat_context_container_element = document.getElementById("chat-context-container");
    if (chat_context_container_element != null)
    {
        let message_press_timer = null;
        let message_press_moved = false;

        chat_context_container_element.addEventListener("touchstart", function(event) {
            if (g_is_client_running_under_touch_device == false)
            {
                return;
            }

            let message_p = event.target.closest(".local-single-chat-message-content-p, .local-client-chat-picture-img");
            if (message_p == null)
            {
                return;
            }

            message_press_moved = false;
            const click_x = event.touches[0].clientX;
            const click_y = event.touches[0].clientY;

            message_press_timer = window.setTimeout(function() {
                if (message_press_moved == true)
                {
                    return;
                }
                // do not bubble to page cleanup, which would close the new menu
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

    // restore saved member list layout, filtering, own actions and avatar size
    let saved_member_list_vertical = null;
    let saved_member_list_scale = null;
    let saved_member_list_neighbors = null;
    saved_member_list_vertical = utils__storage_get("lemon_strip_vertical");
    saved_member_list_scale = utils__storage_get("lemon_strip_scale");
    saved_member_list_neighbors = utils__storage_get("lemon_strip_neighbors");

    if (saved_member_list_vertical == "1")
    {
        document.body.classList.add("msgr-vertical");
    }
    if (saved_member_list_neighbors == "1")
    {
        document.body.classList.add("msgr-neighbors-only");
    }

    // show audio status badges unless the user disabled them
    let saved_audio_availability = null;
    saved_audio_availability = utils__storage_get("lemon_audio_availability");

    if (saved_audio_availability != "0")
    {
        document.body.classList.add("msgr-audio-availability");
    }

    /**
     * @brief sets member avatar, tile and label sizes from the chosen circle size
     *
     * @param number circle_px -> avatar circle diameter in pixels
     *
     * @return void
     */
    function apply_member_list_scale(circle_px)
    {
        document.body.style.setProperty("--msgr-circle", circle_px + "px");
        document.body.style.setProperty("--msgr-item", (circle_px + 16) + "px");
        document.body.style.setProperty("--msgr-member-list", (circle_px + 52) + "px");
        document.body.style.setProperty("--msgr-name", (circle_px <= 46 ? 10 : (circle_px <= 56 ? 11 : 12)) + "px");
    }

    // legacy values 1, 2 and 3 are preset sizes. larger values are pixels
    let restored_circle_px = 46;
    if (saved_member_list_scale != null)
    {
        if (saved_member_list_scale == "2")
        {
            restored_circle_px = 54;
        }
        else if (saved_member_list_scale == "3")
        {
            restored_circle_px = 62;
        }
        else if (parseInt(saved_member_list_scale) >= 38)
        {
            restored_circle_px = parseInt(saved_member_list_scale);
        }
    }
    apply_member_list_scale(restored_circle_px);

    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin", "<input type='button' id='msgr-orientation-button' aria-label='Vertical user list' aria-controls='member-list-body'>" + "<input type='button' id='msgr-neighbors-button' title='show only people in your channel'>" + "<input type='button' id='msgr-me-button' title='you: avatar and account actions'>" + "<input type='range' id='msgr-scale-slider' min='38' max='64' step='2' value='" + restored_circle_px + "' title='people size'>");

    /**
     * @brief matches the toolbar button and settings checkbox to the current member list direction
     *
     * @return void
     */
    function sync_member_list_orientation_controls()
    {
        let vertical = document.body.classList.contains("msgr-vertical");
        let button = document.getElementById("msgr-orientation-button");
        button.setAttribute("aria-pressed", String(vertical));
        button.title = vertical ? "Show users in a row across the top" : "Show users vertically on the left";
        document.getElementById("local-settings-member-list-vertical").checked = vertical;
    }

    sync_member_list_orientation_controls();
    document.getElementById("msgr-orientation-button").onclick = function()
    {
        let vertical_now = document.body.classList.toggle("msgr-vertical");
        utils__storage_set("lemon_strip_vertical", vertical_now ? "1" : "0");
        sync_member_list_orientation_controls();
    };

    document.getElementById("msgr-neighbors-button").onclick = function()
    {
        let neighbors_now = document.body.classList.toggle("msgr-neighbors-only");
        utils__storage_set("lemon_strip_neighbors", neighbors_now ? "1" : "0");
        UI.refresh_member_list_controls();
    };

    document.getElementById("member-list-header").addEventListener("click", function(event)
    {
        let button = event.target.closest("button");
        if (button == null)
        {
            return;
        }
        if (button.id == "pocket-channel-switch")
        {
            let rect = button.getBoundingClientRect();
            member_list_show_channel_switch_menu(8, rect.bottom + 4);
            let first = document.querySelector(".channel-switch-menu-item");
            if (first != null)
            {
                first.focus();
            }
        }
    });
    window.addEventListener("resize", UI.refresh_member_list_controls);
    UI.refresh_member_list_controls();

    // open the local user's menu from the toolbar
    document.getElementById("msgr-me-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");
        let button = this.getBoundingClientRect();
        let bar = document.getElementById("communication-system-head-menu").getBoundingClientRect();
        let menu_x = Math.max(8, Math.min(button.left, window.innerWidth - 200));
        let menu_y = button.bottom + 4;
        if (local_row == null)
        {
            return;
        }

        // a button inside the top bar: the menu opens under the bar, not over it
        if (button.bottom <= bar.bottom && bar.height <= 120)
        {
            menu_y = bar.bottom + 4;
        }

        // open the menu
        let forwarded_event = { stopPropagation: function()
        {
        }, currentTarget: null, which: null };
        UI.connected_user_onmousedown(forwarded_event, true, menu_x, menu_y, local_row, false);
    };

    document.getElementById("msgr-scale-slider").oninput = function()
    {
        let circle_px = parseInt(this.value);
        apply_member_list_scale(circle_px);
        utils__storage_set("lemon_strip_scale", "" + circle_px);
    };

    // refresh connection status, latency, traffic and duration while the card is open
    document.getElementById("communication-system-head-menu").insertAdjacentHTML("afterbegin", "<input type='button' id='msgr-session-button' title='connection and session info'>");

    document.body.insertAdjacentHTML("beforeend", "<div id='msgr-session-card'>" + "<p class='msgr-session-row'><span class='msgr-session-label'>status</span><span id='msgr-session-status'></span></p>" + "<p class='msgr-session-row'><span class='msgr-session-label'>ping</span><span id='msgr-session-ping'></span></p>" + "<p class='msgr-session-row'><span class='msgr-session-label'>sent</span><span id='msgr-session-sent'></span></p>" + "<p class='msgr-session-row'><span class='msgr-session-label'>received</span><span id='msgr-session-received'></span></p>" + "<p class='msgr-session-row'><span class='msgr-session-label'>session</span><span id='msgr-session-uptime'></span></p>" + "</div>");

    /**
     * @brief formats the session traffic count in bytes, kilobytes or megabytes
     *
     * @param number byte_count -> transferred byte count
     *
     * @return string -> size label with one decimal place for kilobytes and megabytes
     */
    function format_byte_count(byte_count)
    {
        if (byte_count >= 1048576)
        {
            return (byte_count / 1048576).toFixed(1) + " MB";
        }
        if (byte_count >= 1024)
        {
            return (byte_count / 1024).toFixed(1) + " KB";
        }
        return byte_count + " B";
    }

    /**
     * @brief updates connection status, latency, traffic totals and uptime from the current session
     *
     * @return void
     */
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

    // close the card when clicking outside it
    document.addEventListener("mousedown", function(event) {
        let session_card = document.getElementById("msgr-session-card");
        if (session_card == null || session_card.classList.contains("msgr-session-card-open") == false)
        {
            return;
        }
        if (event.target.closest("#msgr-session-card") != null || event.target.closest("#msgr-session-button") != null)
        {
            return;
        }

        session_card.classList.remove("msgr-session-card-open");
        clearInterval(session_card_refresh_timer);
    });

    // enable the grid on desktop. touch devices keep the existing layout
    g_layout.panels = {
        channels: document.getElementById("space-devider1"),
        chat: document.getElementById("space-devider2"),
        info: document.getElementById("space-devider-42"),
        input: document.getElementById("space-devider4"),
        toolbar: document.getElementById("communication-system-head-menu")
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

        // capture edit gestures before inner click handlers
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

    // bind the initial conversation tabs before any chats are added
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
    // initialize the connection and bookmark menus
    if (typeof head_menu__init === "function")
    {
        head_menu__init();
    }
    document.getElementById("chat-input-container-text-input").onkeydown = UI.chat_input_on_keydown;
    document.getElementById("chat-input-container-text-input").onbeforeinput = UI.chat_input_on_beforeinput;
    document.getElementById("chat-input-container-text-input").onkeyup = UI.chat_input_on_keyup;
    document.addEventListener("focusin", function(event)
    {
        if (main__is_editable_field(event.target))
        {
            main__chat_input_onfocus();
        }
    });
    document.getElementById("chat-input-container-text-input").onpaste = chat_files__chat_input_onpaste;
    document.getElementById("choose_image_input").onchange = UI.choose_image_input;
    document.getElementById("choose-file-input").onchange = chat_files__choose_chat_file_input_onchange;
    document.getElementById("image-upload-preview-remove").onclick = chat_files__clear_pending_chat_picture;
    document.getElementById("server-settings-general-file-upload-max-size-input").oninput = chat_files__refresh_file_upload_size_warning;
    document.getElementById("server-settings-general-allow-file-uploads-checkbox").onchange = chat_files__refresh_file_upload_size_visibility;
    document.getElementById("server-settings-general-allow-pictures-checkbox").onchange = chat_files__refresh_picture_size_visibility;
    document.getElementById("server-settings-general-country-blocking-checkbox").onchange = server_settings_tab__refresh_country_blocking_visibility;
    document.getElementById("server-settings-general-display-flags-checkbox").onchange = server_settings_tab__refresh_hide_admin_flag_row_visibility;
    document.getElementById("server-settings-general-enable-audio").onchange = server_settings_tab__refresh_audio_connections_row_visibility;
    document.getElementById("server-settings-country-block-select").onchange = server_settings_tab__country_block_select_onchange;
    chat_files__setup_chat_file_drag_and_drop();
    chat_files__setup_chat_file_card_glow();
    chat__setup_public_key_expand();
    chat__setup_public_key_lines_setting();
    chat_files__apply_file_upload_policy_to_ui();
    document.getElementById("add-key-button").onclick = UI.add_key_button_on_click;
    document.getElementById("connect-show-advanced").onchange = UI.show_connection_advanced;
    UI.show_connection_advanced();
    document.getElementById("show-hide-log-button").onclick = UI.show_hide_log_on_click;
    document.querySelector("[data-chat-context-remove-selector-id='channel-0']").onclick = UI.chat_context_remove_onclick;
    UI.setup_channel_properties_controls();
    document.getElementById("close-button-enter-password").onclick = UI.enter_channel_password_closebutton_onclick;
    document.getElementById("close-button-admin-password").onclick = UI.close_admin_password_context_button;
    document.getElementById("close-button-secret-identity-string").onclick = UI.close_button_secret_identity_string_onclick;
    document.getElementById("close-button-identity-string-use").onclick = UI.close_button_identity_string_use;
    document.getElementById("join-passworded-channel-button").onclick = UI.channel_join_button_onclick;
    document.getElementById("admin-password-send-button").onclick = UI.admin_password_send_button_onclick;
    document.getElementById("clear-chat-button").onclick = UI.clear_chat_button_onclick;
    document.getElementById("hide-chat-button").onclick = UI.hide_chat_container;
    document.getElementById("show-hide-member-list-button").onclick = main__show_hide_member_list_button_onclick;
    document.getElementById("communication-system-container").addEventListener("click", main__member_list_blurred_page_onclick);
    document.getElementById("chat-context-select").onchange = main__chat_context_select_onchange;
    document.getElementById("chat-context-select-close").onclick = main__chat_context_select_close_onclick;
    new MutationObserver(main__refresh_chat_context_select).observe(document.getElementById("chat-context-selectors-container"),
        { childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ["style"] });
    new MutationObserver(main__refresh_chat_context_select).observe(document.getElementById("chat-context-container"),
        { childList: true, subtree: true, attributes: true, attributeFilter: ["style"] });
    main__refresh_chat_context_select();
    document.addEventListener("touchstart", main__note_touch, { capture: true, passive: true });
    document.addEventListener("touchend", main__note_touch, { capture: true, passive: true });
    for (let tree_id of ["channel-list-container", "idle-clients-container"])
    {
        for (let event_name of ["mousedown", "click", "dblclick"])
        {
            document.getElementById(tree_id).addEventListener(event_name, main__tree_mouse_on_touch_device);
        }
    }
    document.getElementById("show-admin-password-entry").onclick = UI.enter_admin_password_button_onclick;
    document.getElementById("choose-song-file-input").onchange = UI.choose_song_file_input_onchange;
    document.getElementById("drag-resize-chat").addEventListener('mousedown', UI.drag_resize_chat_onclick, false);
    document.getElementById("stop-song").onclick = UI.stop_song_onclick;
    document.getElementById("save-server-settings-button").onclick = UI.save_server_settings_button_onclick;
    server_settings_tab__setup();
    document.getElementById("server-settings-log-save-button").onclick = UI.server_settings_log_save_button_onclick;
    document.getElementById("server-settings-log-refresh-button").onclick = UI.server_settings_log_refresh_button_onclick;
    document.getElementById("server-settings-log-clear-button").onclick = UI.server_settings_log_clear_button_onclick;
    document.getElementById("enter-server-settings").onclick = UI.enter_server_settings_onclick;
    document.getElementById("local-settings-flat-channel-list").onchange = UI.channel_flatten_toggle_onclick;
    document.getElementById("show-secret-identity-string").onclick = head_menu__identity_button_onclick;
    document.getElementById("export-identity-file-button").onclick = UI.export_identity_file_button_onclick;
    document.getElementById("choose-identity-file-button").onclick = UI.choose_identity_file_button_onclick;
    document.getElementById("identity-file-input").onchange = UI.identity_file_input_onchange;
    document.getElementById("identity-file-confirm-button").onclick = UI.identity_file_confirm_button_onclick;
    document.getElementById("identity-mode-toggle-link").onclick = UI.identity_mode_toggle_onclick;
    // handle identity file drops without forwarding them to chat uploads
    {
        let identity_dialog = document.getElementById("identity-string-use-container-enter");
        identity_dialog.addEventListener("dragover", function(drag_event)
        {
            drag_event.preventDefault();
            drag_event.stopPropagation();
        });
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
    // open identity import from the toolbar
    document.getElementById("identity-menu-export").onclick = head_menu__identity_item_onclick;
    document.getElementById("identity-menu-import").onclick = head_menu__identity_item_onclick;
    document.getElementById("identity-string-use-confirm-button").onclick = UI.identity_string_use_confirm_button;
    document.getElementById("close-button-poke-client").onclick = UI.close_button_poke_client_onclick;
    document.getElementById("close-button-client-info").onclick = UI.close_button_client_info_onclick;
    document.getElementById("server-info-close-button").onclick = function()
    {
        document.getElementById("server-info-dialog").close();
    };
    document.getElementById("server-banner-upload-button").onclick = function()
    {
        document.getElementById("server-banner-file-input").click();
    };
    document.getElementById("server-banner-file-input").onchange = server_info__upload_banner;
    document.getElementById("server-banner-remove-button").onclick = server_info__remove_banner;
    document.getElementById("close-button-set-new-username").onclick = UI.close_button_set_new_username_onclick;
    document.getElementById("close-button-server-settings").onclick = UI.close_button_server_settings_onclick;
    document.getElementById("poke-client-send-button").onclick = UI.poke_client_send_button_onclick;

    // apply the volume slider to the selected sender's playback
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

    // these settings apply only on this device
    UI.setup_local_settings_tabs();
    UI.setup_chat_display_settings();
    document.getElementById("local-settings-show-mp3-button").checked = utils__storage_get("lemon_show_mp3_button") !== "0";
    document.body.classList.toggle("hide-mp3-button", !document.getElementById("local-settings-show-mp3-button").checked);
    channel_tree__setup_icon_preference();
    chat_history__setup();
    chat_reactions__setup();
    chat_replies__setup();
    groups__setup();
    server_emojis__setup();
    moderation__setup();
    channel_groups__setup();
    chat_files__setup_receive_preferences_controls();
    document.getElementById("local-settings-button").onclick = function()
    {
        let panel = document.getElementById("local-settings-container");
        document.getElementById("local-settings-show-avatars").checked = g_show_message_avatars;
        document.getElementById("local-settings-theme-chat-formatting").checked = g_use_theme_chat_formatting;
        chat_files__refresh_receive_preferences_controls();
        document.getElementById("local-settings-rsa-key-bits").value = String(g_rsa_key_bits);
        document.getElementById("local-settings-seen-indicator").checked = g_show_seen_indicator;
        document.getElementById("local-settings-send-seen").checked = g_send_seen_receipts;
        document.getElementById("local-settings-auto-scroll").checked = g_auto_scroll_chat_to_end;
        document.getElementById("local-settings-hide-mic").checked = g_hide_microphone_button;
        document.getElementById("local-settings-hide-mobile-mic").checked = g_hide_mobile_microphone_button;
        document.getElementById("local-settings-show-mp3-button").checked = !document.body.classList.contains("hide-mp3-button");
        document.getElementById("local-settings-sound-effects").checked = g_are_sound_effects_enabled;
        document.getElementById("local-settings-country-flags").checked = (g_show_hide_toggle == false);
        document.getElementById("local-settings-custom-cursor").checked = g_is_custom_cursor_enabled;
        render_mic_mode_controls();
        document.getElementById("local-settings-file-logging").checked = g_is_file_logging_enabled;
        document.getElementById("local-settings-show-log").checked = (document.getElementById("show-hide-log-button").style.display !== "none");
        document.getElementById("local-settings-member-list-vertical").checked = document.body.classList.contains("msgr-vertical");
        document.getElementById("local-settings-member-list-neighbors").checked = document.body.classList.contains("msgr-neighbors-only");
        document.getElementById("local-settings-audio-availability").checked = document.body.classList.contains("msgr-audio-availability");
        populate_microphone_device_list();

        if (panel.style.display === "none")
        {
            panel.classList.remove("closing");
            panel.style.position = "";
            panel.style.margin = "";
            panel.style.left = "";
            panel.style.top = "";

            // clear the inline display value so the theme controls the panel layout
            panel.style.display = "";

            // phone themes fill the screen. center the smaller desktop window after it opens
            if (window.getComputedStyle(panel).position !== "fixed")
            {
                panel.style.position = "fixed";
                panel.style.margin = "0";
                panel.style.left = "0";
                panel.style.top = "0";
                panel.style.left = Math.max(0, Math.round((utils__viewport_css_width() - panel.offsetWidth) / 2)) + "px";
                panel.style.top = Math.max(0, Math.round((utils__viewport_css_height() - panel.offsetHeight) / 2)) + "px";
            }
        }
        else
        {
            close_local_settings();
        }
    };

    // wait for the closing animation before hiding the panel
    /**
     * @brief hides local settings after its closing animation
     *
     * @return void
     */
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

    // show the same voice mode in the dropdown and touch controls
    /**
     * @brief matches microphone controls to the saved mode and hides push-to-talk options in continuous mode
     *
     * @return void
     */
    function render_mic_mode_controls()
    {
        document.getElementById("local-settings-mic-mode").value = g_is_continuous_mic_mode ? "continuous" : "push-to-talk";
        document.getElementById("local-settings-ptt-key").value = g_push_to_talk_key_label;
        document.getElementById("local-settings-ptt-disable-while-typing").checked = g_is_push_to_talk_disabled_while_typing;

        // the key applies only in push to talk mode
        document.getElementById("local-settings-ptt-key-row").style.display = g_is_continuous_mic_mode ? "none" : "";
        document.getElementById("local-settings-ptt-disable-while-typing-row").style.display = g_is_continuous_mic_mode ? "none" : "";

        let segments = document.querySelectorAll("#local-settings-mic-mode-segmented .local-settings-segment");

        for (let x = 0; x < segments.length; x++)
        {
            let is_selected = (segments[x].getAttribute("data-mic-mode") === "continuous") === g_is_continuous_mic_mode;
            segments[x].classList.toggle("segment-selected", is_selected);
        }
    }

    // list available microphones. show a hint until permission exposes device names
    /**
     * @brief lists available audio inputs and selects the saved device when it is still present
     *
     * @return void
     */
    function populate_microphone_device_list()
    {
        let row = document.getElementById("local-settings-mic-device-row");

        if (row == null || navigator.mediaDevices == null || typeof navigator.mediaDevices.enumerateDevices != "function")
        {
            return;
        }

        navigator.mediaDevices.enumerateDevices().then(function(devices)
        {
            // an empty device ID is a placeholder until permission is granted
            let inputs = devices.filter(function(device)
            {
                return device.kind === "audioinput" && device.deviceId !== "";
            });

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
        }).catch(function()
        {
            row.style.display = "none";
        });
    }

    // replace the capture source without disconnecting the shared gain node
    /**
     * @brief replaces an active microphone stream while keeping its current transmit state
     *
     * @return void
     */
    function apply_selected_microphone_device()
    {
        if (g_local_audio_stream == null || navigator.mediaDevices == null)
        {
            return; // save the choice for the next capture if none is active
        }

        let switch_constraints = (g_selected_microphone_device_id != "")
            ? { audio: { deviceId: { ideal: g_selected_microphone_device_id } } }
            : { audio: true };

        navigator.mediaDevices.getUserMedia(switch_constraints).then(function(new_stream)
        {
            let old_track = g_local_audio_stream.getAudioTracks()[0];
            let new_track = new_stream.getAudioTracks()[0];

            // keep the current transmit state on the replacement track
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

    /**
     * @brief saves the microphone mode and stops continuous transmission when switching to push-to-talk
     *
     * @param boolean is_continuous -> true for continuous mode, false for push-to-talk
     *
     * @return void
     */
    function apply_mic_mode(is_continuous)
    {
        g_is_continuous_mic_mode = is_continuous;
        utils__storage_set("lemon_continuous_mic", g_is_continuous_mic_mode ? "1" : "0");

        // stop transmission before changing voice mode
        if (g_is_continuous_mic_mode == false && g_is_continuous_transmission_active == true)
        {
            g_is_continuous_transmission_active = false;
            document.getElementById("microphone-push-to-talk-button-touch-device").classList.remove("mic-continuous-active");
            voice__process_stop_sending_audio();
        }

        render_mic_mode_controls();
    }

    // settings apply immediately. save closes the panel
    document.getElementById("local-settings-close-button").onclick = close_local_settings;
    document.getElementById("local-settings-x-button").onclick = close_local_settings;

    // keep the settings and toolbar controls on the same member list preferences
    document.getElementById("local-settings-member-list-vertical").onchange = function()
    {
        document.body.classList.toggle("msgr-vertical", this.checked);
        utils__storage_set("lemon_strip_vertical", this.checked ? "1" : "0");
        sync_member_list_orientation_controls();
    };

    document.getElementById("local-settings-member-list-neighbors").onchange = function()
    {
        document.body.classList.toggle("msgr-neighbors-only", this.checked);
        utils__storage_set("lemon_strip_neighbors", this.checked ? "1" : "0");
        UI.refresh_member_list_controls();
    };

    document.getElementById("local-settings-audio-availability").onchange = function()
    {
        document.body.classList.toggle("msgr-audio-availability", this.checked);
        utils__storage_set("lemon_audio_availability", this.checked ? "1" : "0");
    };

    // open the local user's avatar and account menu
    document.getElementById("local-settings-avatar-button").onclick = function()
    {
        let local_row = document.querySelector("#channel-list-container .connected-local-client");

        if (local_row == null)
        {
            utils__custom_alert("connect first to change your avatar");
            return;
        }

        close_local_settings();

        let forwarded_event = { stopPropagation: function()
        {
        }, currentTarget: null, which: null };
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
        android_host__refresh_message_avatars();
        if (this.checked)
        {
            channel_tree__start_avatar_prefetch();
        }
    };

    document.getElementById("local-settings-theme-chat-formatting").onchange = function()
    {
        g_use_theme_chat_formatting = this.checked;
        utils__storage_set("lemon_use_theme_chat_formatting", this.checked ? "1" : "0");
        chat__apply_theme_formatting();
    };

    // apply key size changes to the next identity generation
    document.getElementById("local-settings-rsa-key-bits").onchange = function()
    {
        let chosen_bits = parseInt(this.value);

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(chosen_bits) < 0)
        {
            return;
        }

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

        if (G_ALLOWED_RSA_KEY_BITS.indexOf(target_bits) < 0)
        {
            return;
        }

        keys__hide_rsa_key_too_weak_dialog();
        utils__custom_alert("creating a " + target_bits + "-bit identity key, this can take a while ...");

        if (typeof Android !== "undefined" && Android != null)
        {
            // only native storage owns this seed. a page must not derive a different local identity
            if (UI.identity_apply_passphrase(null, target_bits) === true)
            {
                connection__request_connect("button");
            }
            return;
        }
        if (typeof g_identity_string !== "string" || g_identity_string.length < 199)
        {
            utils__custom_alert("The current identity is not ready. Wait for it to finish or import your identity backup before changing its key size.");
            return;
        }
        // retry with the selected identity without changing the next key preference
        connection__request_identity(g_identity_string, target_bits);
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

    document.getElementById("local-settings-custom-cursor").onchange = function()
    {
        g_is_custom_cursor_enabled = this.checked;
        utils__storage_set("lemon_custom_cursor", this.checked ? "1" : "0");
        main__apply_custom_cursor_setting();
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

    document.getElementById("local-settings-hide-mobile-mic").onchange = function()
    {
        g_hide_mobile_microphone_button = this.checked;
        utils__storage_set("lemon_hide_mobile_mic", this.checked ? "1" : "0");
        voice__update_microphone_button();
    };

    document.getElementById("local-settings-show-mp3-button").onchange = function()
    {
        document.body.classList.toggle("hide-mp3-button", !this.checked);
        utils__storage_set("lemon_show_mp3_button", this.checked ? "1" : "0");
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

    document.getElementById("local-settings-ptt-disable-while-typing").onchange = function()
    {
        g_is_push_to_talk_disabled_while_typing = this.checked;
        utils__storage_set("lemon_ptt_disable_while_typing", this.checked ? "1" : "0");
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

    // the audio connection preference: stored locally, applied at once within the server's policy
    document.getElementById("local-settings-audio-connection").value = g_audio_connection_preference;
    document.getElementById("local-settings-audio-connection").onchange = function()
    {
        audio_transport__set_preference(this.value);
    };

    // press the button, then press the key you want. escape cancels
    document.getElementById("local-settings-ptt-key").onclick = function()
    {
        let key_button = this;
        key_button.value = "press a key…";

        /**
         * @brief records the next push-to-talk key or cancels key selection on Escape
         *
         * @param KeyboardEvent event -> captured key press
         *
         * @return void
         */
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
                catch (e)
                {
                }
            }

            key_button.value = g_push_to_talk_key_label;
        };

        window.addEventListener("keydown", capture_key, true);
    };

    // hide toolbar copies of controls now available in local settings
    document.getElementById("sound-effects-button").style.display = "none";
    document.getElementById("hide-show-flags-button").style.display = "none";
    document.getElementById("microphone-always-broadcasting-audio-button").style.display = "none";

    // restore the saved sound preference after state initialization
    let stored_sound = utils__storage_get("lemon_sound_effects");
    if (stored_sound != null)
    {
        g_are_sound_effects_enabled = (stored_sound === "1");
        sounds__apply_sound_effects_muted();
    }
    // restore the saved cursor preference
    let stored_custom_cursor = utils__storage_get("lemon_custom_cursor");
    if (stored_custom_cursor != null)
    {
        g_is_custom_cursor_enabled = (stored_custom_cursor === "1");
    }
    main__apply_custom_cursor_setting();
    main__sync_toolbar_state_classes(); // initialize toolbar state classes for themed icons
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
        let item = document.getElementsByClassName("server-settings-tab-li")[i];
        item.onclick = UI.server_settings_tab_li_onclick;
        item.tabIndex = 0;
        item.setAttribute("role", "button");
        item.onkeydown = function(event)
        {
            if (event.key === "Enter" || event.key === " ")
            {
                event.preventDefault();
                event.currentTarget.click();
            }
        };
    }


    // keep resized identity dialogs inside the viewport without losing their dragged position
    /**
     * @brief moves visible dragged identity dialogs back inside the viewport
     *
     * @return void
     */
    let fit_identity_dialogs = function()
    {
        for (let dialog of document.querySelectorAll(".identity-dialog"))
        {
            if (dialog.offsetWidth === 0 || dialog.style.position !== "fixed")
            {
                continue;
            }
            let max_left = Math.max(12, utils__viewport_css_width() - dialog.offsetWidth - 12);
            let max_top = Math.max(12, utils__viewport_css_height() - dialog.offsetHeight - 12);
            dialog.style.left = Math.min(Math.max(12, dialog.offsetLeft), max_left) + "px";
            dialog.style.top = Math.min(Math.max(12, dialog.offsetTop), max_top) + "px";
        }
    };
    let identity_dialog_resize_observer = new ResizeObserver(fit_identity_dialogs);
    for (let dialog of document.querySelectorAll(".identity-dialog"))
    {
        identity_dialog_resize_observer.observe(dialog);
    }
    window.addEventListener("resize", fit_identity_dialogs);

    // drag popups by their title bars. recenter them when reopened
    (function()
    {
        let dragging = null;
        let ox = 0;
        let oy = 0;
        let bars = document.querySelectorAll('[id="menu-bar-container"], #server-info-header, .groups-titlebar');
        let i = 0;
        let dialog = null;

        /**
         * @brief clears the dragged dialog position so its normal layout can place it again
         *
         * @param HTMLElement dialog -> identity dialog whose fixed position is cleared
         *
         * @return void
         */
        let clear_drag_pin = function(dialog)
        {
            dialog.style.position = "";
            dialog.style.margin = "";
            dialog.style.left = "";
            dialog.style.top = "";
            dialog.style.right = "";
            dialog.style.bottom = "";
        };

        // recenter a popup when it becomes visible
        let recenter_observer = new MutationObserver(function(mutations)
        {
            let i = 0;
            let container = null;
            let dialog = null;
            let dw = 0;
            let dh = 0;

            for (i = 0; i < mutations.length; i++)
            {
                container = mutations[i].target;
                if (container.style.display == "none" || container.style.display == "")
                {
                    continue;
                }
                dialog = container.querySelector('[data-recenter-on-open="true"]');
                if (dialog != null)
                {
                    // center the dialog even when its overlay has no size
                    clear_drag_pin(dialog);
                    dialog.style.position = "fixed";
                    dialog.style.margin = "0";
                    // measure layout size so opening animations do not shift the center
                    dw = dialog.offsetWidth;
                    dh = dialog.offsetHeight;
                    dialog.style.left = Math.max(0, Math.round((utils__viewport_css_width() - dw) / 2)) + "px";
                    dialog.style.top = Math.max(0, Math.round((utils__viewport_css_height() - dh) / 2)) + "px";
                }
            }
        });

        for (i = 0; i < bars.length; i++)
        {
            dialog = bars[i].parentElement;
            if (dialog.tagName !== "DIALOG")
            {
                dialog.setAttribute("data-recenter-on-open", "true");
                if (dialog.parentElement != null)
                {
                    recenter_observer.observe(dialog.parentElement, { attributes: true, attributeFilter: ["style"] });
                }
            }

            else
            {
                dialog.addEventListener("close", function()
                {
                    clear_drag_pin(this);
                });
            }

            // pointer events support mouse, touch and pen. prevent scrolling during a drag
            bars[i].style.touchAction = "none";
            bars[i].addEventListener("pointerdown", function(e)
            {
                let dialog = this.parentElement;
                let rect = null;

                if (e.button !== 0 || e.target.closest(".close-button") != null)
                {
                    return;
                }

                // no moving on phones
                if (main__is_current_theme_for_phones() == true)
                {
                    return;
                }
                rect = dialog.getBoundingClientRect();
                if (rect.width == 0 && rect.height == 0)
                {
                    return;
                }
                dragging = dialog;
                ox = e.clientX - rect.left;
                oy = e.clientY - rect.top;
                dialog.style.position = "fixed";
                dialog.style.margin = "0";
                if (dialog.tagName === "DIALOG")
                {
                    dialog.style.right = "auto";
                    dialog.style.bottom = "auto";
                }
                dialog.style.left = utils__position_px(rect.left);
                dialog.style.top = utils__position_px(rect.top);
                e.preventDefault();
            });
        }
        document.addEventListener("pointermove", function(e)
        {
            let left = 0;
            let top = 0;

            if (dragging == null)
            {
                return;
            }
            left = e.clientX - ox;
            top = e.clientY - oy;
            if (dragging.tagName === "DIALOG")
            {
                if (!dragging.open)
                {
                    dragging = null;
                    return;
                }
                left = Math.max(0, Math.min(left, utils__viewport_width() - dragging.offsetWidth * g_position_scale));
                top = Math.max(0, Math.min(top, utils__viewport_height() - dragging.offsetHeight * g_position_scale));
            }
            dragging.style.left = utils__position_px(left);
            dragging.style.top = utils__position_px(top);
        });
        document.addEventListener("pointerup", function()
        {
            dragging = null;
        });
        document.addEventListener("pointercancel", function()
        {
            dragging = null;
        });
        window.addEventListener("blur", function()
        {
            dragging = null;
        });
        window.addEventListener("resize", function()
        {
            for (let bar of bars)
            {
                let dialog = bar.parentElement;
                let rect = dialog.getBoundingClientRect();
                if (dialog.tagName === "DIALOG" && dialog.open && dialog.style.left !== "")
                {
                    dialog.style.left = utils__position_px(Math.max(0, Math.min(rect.left, utils__viewport_width() - dialog.offsetWidth * g_position_scale)));
                    dialog.style.top = utils__position_px(Math.max(0, Math.min(rect.top, utils__viewport_height() - dialog.offsetHeight * g_position_scale)));
                }
            }
        });

        // keep the upper left corner in place when resizing Server groups
        document.getElementById("groups-dialog").addEventListener("mousedown", function(event)
        {
            let rect = this.getBoundingClientRect();
            if (event.button === 0 && event.clientX >= rect.right - 18 && event.clientY >= rect.bottom - 18)
            {
                this.style.position = "fixed";
                this.style.margin = "0";
                this.style.right = "auto";
                this.style.bottom = "auto";
                this.style.left = utils__position_px(rect.left);
                this.style.top = utils__position_px(rect.top);
            }
        });
    })();

    UI.setup_theme_menu_positioning();
    for (let i = 0; i < document.getElementsByClassName("choose-theme-item").length; i++)
    {
        document.getElementsByClassName("choose-theme-item")[i].onclick = UI.choose_theme_item_onclick;
    }

    // touch devices only show themes marked for mobile use
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

    // resume playback on a user gesture. keep listening until audio_context exists and runs
    /**
     * @brief resumes suspended audio and removes gesture listeners once playback is allowed
     *
     * @return void
     */
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
    g_video_worker = audio_opus_glue__create_new_webworker_in_same_file("video_worker");

    // initialize video controls after the video worker
    video__init_ui();

    // restore the saved identity only when served configuration enables persistence
    let persist_identity_enabled = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.persist_identity === true);

    // apply the server's avatar policy and upload limit
    g_server_policy.is_avatar_upload_allowed = (typeof window.__SERVER_CONFIG__ !== "undefined" && window.__SERVER_CONFIG__ != null && window.__SERVER_CONFIG__.is_avatar_upload_allowed === true);
    if (g_server_policy.is_avatar_upload_allowed == true && typeof window.__SERVER_CONFIG__.avatar_max_size === "number" && window.__SERVER_CONFIG__.avatar_max_size > 0)
    {
        g_server_policy.avatar_max_size = window.__SERVER_CONFIG__.avatar_max_size;
    }

    const saved_identity = persist_identity_enabled
        ? identity_file__read_saved(utils__storage_get("lemon_identity_record_v1"), utils__storage_get("lemon_identity_string"))
        : {kind: "missing", passphrase: null};

    // complete normal page initialization even when the saved identity needs recovery
    // no old seed or corrupt record is silently replaced with a newly generated identity
    if (typeof Android === "undefined")
    {
        if (saved_identity.kind === "recovery")
        {
            setTimeout(function()
            {
                connection__report_connection_status("idle", saved_identity.reason);
                UI.identity_show_saved_recovery(saved_identity);
            }, 0);
        }
        else
        {
            connection__request_identity(saved_identity.passphrase, saved_identity.kind === "ready" ? saved_identity.rsa_key_bits : g_rsa_key_bits);
        }
    }

    g_minimp3_worker.postMessage({
        type: "init"
    });

    // apply configured styles and the default theme

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

    main__apply_startup_theme();

    // apply Android mode after restoring the theme
    main__apply_theme_for_app_mode();

    // restore the flat channel preference before theme changes can expose it
    g_is_channel_list_flattened = (utils__storage_get("lemon_channels_flat") == "1");
    UI.apply_channel_flatten_state();

    if (g_is_running_in_android_webview)
    {
        g_send_go_to_idle_mode_request = android_js_bridge.send_go_to_idle_mode_request_android;
        g_send_come_from_idle_mode_request = android_js_bridge.send_come_from_idle_mode_request_android;
        g_mark_call_accept_presence = android_js_bridge.mark_call_accept_presence_android;
        g_set_username_on_connect = android_js_bridge.set_username_on_connect_android;
        g_accept_current_settings_from_android = android_js_bridge.accept_current_settings_from_android;
        g_request_loopback_reattach = android_js_bridge.request_loopback_reattach_android;
        g_show_connection_phase = android_js_bridge.show_connection_phase_android;

        // apply native settings received before page initialization
        if (typeof g_pending_android_settings === "string" && g_pending_android_settings != null)
        {
            console.log("connect-path: applying settings that arrived before onload");
            g_accept_current_settings_from_android(g_pending_android_settings);
            g_pending_android_settings = null;
        }

        // expose Android mode to the theme styles
        document.body.classList.add("android-app");

        // Android assets have no served config. allow avatar controls until server policy arrives
        g_server_policy.is_avatar_upload_allowed = true;

        document.getElementById("import-identity-button").style.display = "none";

        // show the toolbar settings button while connected. confirm before changing connection details
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

        /**
         * @brief hides the warning shown before opening Android settings
         *
         * @return void
         */
        let close_settings_warning = function() {
            document.getElementById("android-settings-warning-container").style.display = "none";
        }
        document.getElementById("android-settings-warning-cancel-button").onclick = close_settings_warning;
        document.getElementById("close-button-android-settings-warning").onclick = close_settings_warning;

        // show connection settings on the disconnected page
        document.getElementById("android-server-settings-container").style.display = "";
        document.getElementById("android-server-settings-button").onclick = function() {
            Android.JavaExportGoToSettings();
        }

        Android.JavaExportRequestCurrentSettingsFromAndroid();

        // native settings provide the connection target
    }
    else if (g_are_server_details_predefined == true)
    {
        // start autoconnect using the page's configured target
        connection__request_connect("settings");
    }

    // start the connect screen reveal deadline and apply its themed background
    connection__set_connect_holdback_loader_visible(true);
    g_connect_holdback_started = new Date().valueOf();
    g_connect_holdback_deadline = g_connect_holdback_started + 2500;
    setTimeout(connection__connect_holdback_check, 250);

    // manual browser connections need no startup spinner
    if (g_is_running_in_android_webview == false && g_is_autoconnect_without_user_action_active == false)
    {
        connection__reveal_connect_page();
    }

    connection__start_connection_status_ticker();
    connection__connect_loop();
}

// workers and Node do not run page initialization

if (typeof window !== 'undefined')
{
    window.onload = main__window_onload;
}

// the export seam for the headless node build:
return {
    server_msg: server_msg,
    client_msg: client_msg,
    // native entry points for the shared client
    android_js_bridge: android_js_bridge,

    // initialize transport and logging before connecting
    init_node_runtime: android_host__init_node_runtime,

    // allow connections or close the socket and pause retries
    node_set_connection_wanted: android_host__node_set_connection_wanted,
    /**
     * @brief starts the shared connection flow for a manual Node connect request
     *
     * @return boolean -> true after submitting the request
     */
    node_request_connect: function()
    {
        connection__request_connect("button");
        return true;
    },
    /**
     * @brief disconnects the shared client and stops automatic retries
     *
     * @return boolean -> true after applying the disconnect request
     */
    node_disconnect_by_user: function()
    {
        connection__disconnect_by_user();
        return true;
    },
    /**
     * @brief sends an upload through the active authenticated Node connection
     *
     * @param object request -> upload request for the connection worker
     *
     * @return Promise<boolean> -> false when disconnected or the send fails, true when accepted
     */
    node_forward_upload_request: function(request)
    {
        if (!g_is_authenticated || g_worker_connection_ended)
        {
            return Promise.resolve(false);
        }
        return connection__send_upload_message(request);
    },
    node_handle_call_action: android_host__handle_call_action,
    node_cancel_upload_request: connection__cancel_upload_send,
    node_delivery_context: chat__delivery_context,
    /**
     * @brief updates saved local ignore state only for the matching live identity
     *
     * @param object arguments_value -> public key, identity hash and boolean ignored value
     *
     * @return boolean -> true when the matched client state was saved
     */
    node_set_local_ignore: function(arguments_value)
    {
        if (!arguments_value || typeof arguments_value.ignored !== "boolean")
        {
            return false;
        }
        const client = g_client_list.find(function(candidate)
        {
            return candidate.public_key && candidate.public_key === arguments_value.public_key && candidate.identity_hash === arguments_value.identity_hash;
        });
        if (!client)
        {
            return false;
        }
        client.is_ignored_by_local_client = arguments_value.ignored;
        chat__remember_peer_state();
        return true;
    },
    /**
     * @brief marks a validated current conversation as read
     *
     * @param object arguments_value -> conversation identity and receipt preference from the page
     *
     * @return boolean true when the conversation was found and marked read
     */
    node_read_conversation: function(arguments_value)
    {
        if (!arguments_value || !g_node_has_attached_ui)
        {
            return false;
        }
        // match a channel read to its declared conversation
        if (Number.isSafeInteger(arguments_value.channel_id) && arguments_value.channel_id >= 0)
        {
            if (arguments_value.context_id !== "chat-context-channel-" + arguments_value.channel_id)
            {
                return false;
            }
            chat__clear_channel_unread_count(arguments_value.channel_id);
            return true;
        }
        // resolve a live person by both key and identity
        const client = g_client_list.find(function(candidate)
        {
            return candidate.public_key && candidate.public_key === arguments_value.public_key && candidate.identity_hash === arguments_value.identity_hash;
        });
        if (client)
        {
            if (arguments_value.context_id !== chat__peer_state_key(client))
            {
                return false;
            }
            chat__clear_unread_count(client.client_id, true, arguments_value.send_seen_receipts === true);
            return true;
        }
        // find retained unread state when the person is no longer connected
        const peer_context = arguments_value.identity_hash ? "identity:" + arguments_value.identity_hash : (typeof arguments_value.public_key === "string" && arguments_value.public_key ? "key:" + arguments_value.public_key : "");
        const state = chat_peer_local_state.get(peer_context);
        if (state && peer_context && arguments_value.context_id === peer_context)
        {
            state.unread_count = 0;
            chat__update_app_unread_badge();
            return true;
        }
        return false;
    },
    /**
     * @brief stops connection retries and exposes a bounded delivery failure message
     *
     * @param string reason -> failure text or an empty value to use the capacity warning
     *
     * @return void
     */
    node_fail_delivery: function(reason)
    {
        android_host__node_set_connection_wanted(false);
        connection__report_connection_status("error", String(reason || "Pending message capacity exceeded. Reopen the chat.").slice(0, 240));
    },
    /**
     * @brief sets the Node notification callback or clears it for a nonfunction value
     *
     * @param function|null callback -> listener for notification requests
     *
     * @return void
     */
    set_notification_listener: function(callback)
    {
        node_notification_listener = typeof callback === "function" ? callback : null;
    },
    /**
     * @brief copies current session, identity, peers and unread counts for an attached page
     *
     * @return object -> detached JSON snapshot with authenticated false during recovery
     */
    get_ui_session_snapshot: function()
    {
        return JSON.parse(JSON.stringify(
        {
            session_id: node_session_id,
            public_key_string: g_rsa_public_key_string,
            identity_revision: g_identity_revision,
            local_client_id: g_local_client_id,
            current_channel_id: g_current_channel_id,
            authenticated: g_is_authenticated && !g_fast_reconnect.in_progress && g_connection_status.state === "connected",
            phase: g_connection_status.state,
            server_policy: g_server_policy,
            current_call: node_current_call,
            peer_states: chat__snapshot_peer_states(),
            channel_counts: Object.keys(g_channel_unread_counts).map(function(key)
            {
                return {channel_id: Number(key), unread_count: g_channel_unread_counts[key]};
            }),
            clients: g_client_list,
            channels: g_channel_list
        }));
    },

    /**
     * @brief sets the username requested at login
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
     * @brief encrypts and sends a request from the local page
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
     * @brief registers the decrypted frame listener used for page replay
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
     * @brief adds a listener for connection status changes
     *
     * @param function callback -> callback(status)
     *
     * @return void
     */
    set_connection_status_listener: function(callback)
    {
        if (typeof callback === "function")
        {
            g_connection_status_listeners.push(callback);
        }
    },

    /**
     * @brief registers the listener that publishes total unread count changes
     *
     * @param function callback -> callback(total), null clears it
     *
     * @return void
     */
    set_unread_listener: function(callback)
    {
        if (typeof callback === "function")
        {
            node_unread_listeners.push(callback);
        }
        /**
         * @brief notifies registered Node unread listeners without letting one failure stop the others
         *
         * @param number total -> current unread message total
         *
         * @return void
         */
        g_node_unread_listener = function(total)
        {
            for (const listener of node_unread_listeners)
            {
                try
                {
                    listener(total);
                }
                catch (error)
                {
                    console.warn("unread listener failed");
                }
            }
        };
    },

    /**
     * @brief registers the native incoming call handler
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
     * @brief applies page visibility and native idle policy without marking messages read
     *
     * @param boolean is_attached -> true when the page is visible
     * @param boolean is_from_host -> true when native lifecycle state owns the update
     * @param preserve_idle true to keep the current idle state
     * @param force_idle true to idle even after a previous background transition
     *
     * @return void
     */
    node_set_ui_attached: function(is_attached, is_from_host, preserve_idle, force_idle)
    {
        if (is_from_host === true)
        {
            node_ui_visibility_from_host = true;
        }
        else if (node_ui_visibility_from_host == true)
        {
            // a background loopback reconnect cannot override native visibility
            return;
        }

        let was_attached = g_node_has_attached_ui;
        g_node_has_attached_ui = (is_attached === true);
        if (is_from_host === true)
        {
            node_ui_forced_idle = !g_node_has_attached_ui && (node_ui_forced_idle || force_idle === true);
        }

        // retain forced idle after a task swipe, including before authentication
        if (node_ui_forced_idle && !g_node_has_attached_ui)
        {
            android_host__enter_deep_idle(true);
            return;
        }

        // move an unattended authenticated session to idle so it can receive calls
        if (preserve_idle !== true && g_is_authenticated == true && was_attached != g_node_has_attached_ui)
        {
            android_host__node_apply_idle_for_ui_state();
        }
    },

    /**
     * @brief applies network availability reported by Android
     *
     * @param boolean is_available -> true while wifi or mobile data is on
     *
     * @return void
     */
    node_set_device_network: function(is_available)
    {
        g_device_has_network = (is_available === true);

        // update disconnected status immediately when network access is lost
        if (g_device_has_network === false && g_is_authenticated == false)
        {
            g_last_disconnect_reason = "no network connection (wifi and mobile data are off)";
            connection__report_connection_status(g_connection_status.state, g_last_disconnect_reason);
        }
    },

    /**
     * @brief the current connection status
     *
     * @return object a copy of the current connection status
     */
    get_connection_status: function()
    {
        return Object.assign({}, g_connection_status);
    },

    /**
     * @brief applies an explicit Connect request after native settings are available
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
     * @brief reads the cached authentication frame for the current session
     *
     * @return string|null the frame, null before authentication
     */
    get_auth_frame: function()
    {
        return g_node_cached_auth_frame;
    },

    /**
     * @brief adds a callback invoked after shared message processing
     *
     * @param function callback -> callback(message_type, had_error)
     *
     * @return void
     */
    set_on_message_processed: function(callback)
    {
        if (typeof callback === "function")
        {
            g_node_message_listeners.push(callback);
        }
    },

    /**
     * @brief copies the shared state so the host cannot mutate the live session
     *
     * @return object the client and channel lists, chat contexts, groups, icons, the current channel and its keys, the local ids and the connection flags
     */
    read_state: function()
    {
        return {
            g_client_list: JSON.parse(JSON.stringify(g_client_list)),
            g_channel_list: JSON.parse(JSON.stringify(g_channel_list)),
            g_chat_context_array: JSON.parse(JSON.stringify(g_chat_context_array)),
            g_map_client_id_to_array_index: new Map(g_map_client_id_to_array_index),
            g_offline_client_list: JSON.parse(JSON.stringify(g_offline_client_list)),
            g_chat_message_author_public_keys: JSON.parse(JSON.stringify(g_chat_message_author_public_keys)),
            g_groups: JSON.parse(JSON.stringify(g_groups)),
            group_access: JSON.parse(JSON.stringify(groups_window.access)),
            channel_groups: JSON.parse(JSON.stringify(channel_groups.catalog)),
            g_icons: JSON.parse(JSON.stringify(g_icons)),
            current_channel_id: g_current_channel_id,
            current_channel_keys: g_current_channel_keys == null ? null : JSON.parse(JSON.stringify(g_current_channel_keys)),
            // expose media key operations to the test harness
            media_session_layer: keys__media_session_layer(),
            media_server_key_layers: keys__media_server_key_layers(),
            local_client_id: g_local_client_id,
            g_local_username: g_local_username,
            g_current_chat_context_id: g_current_chat_context_id,
            g_chat_message_receiver_type: g_chat_message_receiver_type,
            g_is_client_list_retrieved: g_is_client_list_retrieved,
            g_is_channel_list_retrieved: g_is_channel_list_retrieved,
            // current authenticated session state
            g_is_authenticated: g_is_authenticated,
            g_is_microphone_always_on: g_is_microphone_always_on,
            g_have_received_android_settings: g_have_received_android_settings
        };
    }
};
}));
