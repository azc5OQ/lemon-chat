// provides channel and client lookups, row markup and avatar loading

// keep the current or first visible row in place when other rows change
/**
 * @brief applies a server update and restores a visible channel row's scroll position
 *
 * @param function handler -> server message handler called with server_msg as this
 * @param object message -> server frame passed to the handler
 *
 * @return * -> the handler's return value
 */
function channel_tree__apply_server_update(handler, message)
{
    let container = G_HAS_DOM ? document.getElementById("channel-list-container") : null;
    let anchors = [];
    if (container != null && container.clientHeight > 0)
    {
        let viewport = container.getBoundingClientRect();
        let rows = container.querySelectorAll(".single-channel, .connected-client");
        for (let row of rows)
        {
            let bounds = row.getBoundingClientRect();
            if (bounds.height <= 0 || bounds.bottom <= viewport.top || bounds.top >= viewport.bottom)
            {
                continue;
            }
            let attribute = row.classList.contains("single-channel") ? "data-channel-id" : "data-connected-client-id";
            let id = row.getAttribute(attribute);
            let anchor = { attribute: attribute, id: id, top: bounds.top };
            if (attribute === "data-channel-id" && Number(id) === g_current_channel_id)
            {
                anchors.unshift(anchor);
            }
            else
            {
                anchors.push(anchor);
            }
        }
    }
    try
    {
        return handler.call(server_msg, message);
    }
    finally
    {
        keys__check_channel_key_recipients();

        // update the people counts
        channel_tree__refresh_people_counts();
        for (let anchor of anchors)
        {
            let row = container.querySelector('[' + anchor.attribute + '="' + anchor.id + '"]');
            if (row == null || row.getBoundingClientRect().height <= 0)
            {
                continue;
            }
            container.scrollTop += row.getBoundingClientRect().top - anchor.top;
            break;
        }
    }
}

// remove every copy of a client row and its list entry. repeated connection
// notices must not leave an older copy behind when that client disconnects
/**
 * @brief removes a client from the rows and rebuilds the client ID index
 *
 * @param number|string client_id -> client ID to remove
 *
 * @return void
 */
function channel_tree__forget_client(client_id)
{
    client_id = Number(client_id);
    if (typeof document !== "undefined")
    {
        for (let row of document.querySelectorAll('.connected-client[data-connected-client-id="' + client_id + '"]'))
        {
            row.remove();
        }
    }
    g_map_client_id_to_array_index.clear();
    let kept = 0;
    for (let client of g_client_list)
    {
        if (client.client_id !== client_id)
        {
            g_client_list[kept] = client;
            g_map_client_id_to_array_index.set(client.client_id, kept);
            kept++;
        }
    }
    g_client_list.length = kept;
}

// state private to this file
var PREFETCH_AVATARS_AUTOMATICALLY = true;

var AVATAR_PREFETCH_INTERVAL_MS = 300;

// true once the two controls of the country flag setting have their change listeners
var is_flag_preference_listener_added = false;

// client and channel lookups

/**
 * @brief icon object from g_icons by id
 *
 * @param number icon_id -> the icon id
 *
 * @return object|null the icon, null when not found
 */
function channel_tree__get_icon_by_icon_id(icon_id)
{
    for (var i = 0; i < g_icons.length; i++)
    {
        if (g_icons[i].id == icon_id)
        {
            return g_icons[i];
        }
    }
    return null;
}

/**
 * @brief group object from g_groups by id
 *
 * @param number group_id -> the group id
 *
 * @return object|null the group, null when not found
 */
function channel_tree__get_group_by_group_id(group_id)
{
    for (var i = 0; i < g_groups.length; i++)
    {
        if (g_groups[i].group_id == group_id)
        {
            return g_groups[i];
        }
    }
    return null;
}

/**
 * @brief returns direct child channels or the channels marked as roots
 *
 * @param array channels -> the channel list to search
 * @param number parent_channel_id -> the parent, or the marker value for the roots
 *
 * @return array the child channel objects
 */
function channel_tree__get_channels_by_channel_parent_id(channels, parent_channel_id)
{
    let result = [];
    for (var i = 0; i < channels.length; i++)
    {
        if (parent_channel_id == g_ROOT_LEVEL_PARENT_MARKER)
        {
            // match root channels by the explicit flag because their unsigned parent ID cannot represent minus one
            if (channels[i].is_root_channel == true)
            {
                result.push(channels[i]);
            }
        }
        else if (channels[i].parent_channel_id == parent_channel_id)
        {
            result.push(channels[i]);
        }
    }
    return result;
}

/**
 * @brief channel object from the given channel list by id
 *
 * @param array channel_list_a -> the channel list to search
 * @param number channel_id -> the channel id
 *
 * @return object|null the channel, null when not found
 */
function channel_tree__get_channel_by_id(channel_list_a, channel_id)
{
    let result = null;

    for (let x = 0; x < channel_list_a.length; x++)
    {
        if (channel_list_a[x].channel_id == channel_id)
        {
            result = channel_list_a[x];
            break;
        }
    }

    return result;
}

/**
 * @brief index of a channel in the given channel list by id
 *
 * @param array channel_list_a -> the channel list to search
 * @param number channel_id -> the channel id
 *
 * @return number the index, -1 when not found
 */
function channel_tree__get_channel_index_in_array_by_channel_id(channel_list_a, channel_id)
{
    let result = -1;

    for (let x = 0; x < channel_list_a.length; x++)
    {
        if (channel_list_a[x].channel_id == channel_id)
        {
            result = x;
            break;
        }
    }

    return result;
}

/**
 * @brief finds a channel depth by following its parent chain
 *
 * @param number channel_id -> the channel to measure
 * @param array channels -> the channel list to walk
 *
 * @return number the root depth is zero. a broken or cyclic chain returns the depth reached
 */
function channel_tree__get_indentation_level(channel_id, channels)
{
    let result = 0;
    let is_root_channel_found = false;
    let channel_id_currently_looking_for = channel_id;

    let safety_counter = 0;

    while (is_root_channel_found == false)
    {
        // bound the parent walk so malformed or cyclic channel data cannot loop forever
        if (safety_counter > channels.length)
        {
            console.error("channel_tree__get_indentation_level: could not reach a root channel for channel_id " + channel_id + " - channel data looks invalid");
            return result;
        }
        safety_counter++;

        let was_channel_found = false;
        for (let i = 0; i < channels.length; i++)
        {
            if (channels[i].channel_id == channel_id_currently_looking_for)
            {
                was_channel_found = true;
                if (channels[i].is_root_channel == true)
                {
                    is_root_channel_found = true;
                }
                else
                {
                    result++;
                    channel_id_currently_looking_for = channels[i].parent_channel_id;
                }
                break;
            }
        }

        // stop when a parent is missing from the channel list
        if (was_channel_found == false)
        {
            console.error("channel_tree__get_indentation_level: channel_id " + channel_id_currently_looking_for + " not found while walking parents - channel data looks invalid");
            return result;
        }
    }
    return result;
}

/**
 * @brief checks the admin status sent by the server
 *
 * @return boolean true for an admin
 */
function channel_tree__is_local_client_admin()
{
    return g_is_authenticated && g_is_local_client_admin;
}

/**
 * @brief client object from g_client_list via the id -> index map
 *
 * @param number|string client_id -> the client id, a string is parsed to a number
 *
 * @return object|undefined the client, undefined when the id is not there
 */
function channel_tree__get_client_by_client_id(client_id)
{
    let actual_client_id = null;
    if (typeof client_id === "string")
    {
        actual_client_id = parseInt(client_id);
    }
    else
    {
        actual_client_id = client_id;
    }

    let result = null;

    let client_index = g_map_client_id_to_array_index.get(actual_client_id);
    result = g_client_list[client_index];

    return result;
}

/**
 * @brief reads a client's array index from the client ID map
 *
 * @param number client_id -> the client id, must be a number
 *
 * @return number the index, -1 when the id is not there
 */
function channel_tree__get_client_index_in_array_by_client_id(client_id)
{
    let index = g_map_client_id_to_array_index.get(client_id);

    // return the sentinel expected by callers when the map has no entry
    if (index === undefined)
    {
        // a nonempty list with an empty map indicates inconsistent refresh state
        console.warn("client lookup miss: no entry for client_id " + client_id + ", client list holds " + g_client_list.length);

        return -1;
    }

    return index;
}

/**
 * @brief username for a client id
 *
 * @param number client_id -> the client id
 *
 * @return string|undefined the username, undefined when the client is not in g_client_list
 */
function channel_tree__get_username_by_client_id(client_id)
{
    let result = "";
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i].client_id == client_id)
        {
            result = g_client_list[i].username;
            return result;
        }
    }
}

/**
 * @brief returns the client's current name or the supplied name when the client is absent
 *
 * @param client_id client to find
 * @param fallback_username name to use when the client is absent
 *
 * @return string -> the current name, the supplied fallback, or an empty string
 */
function channel_tree__get_display_name_by_client_id(client_id, fallback_username)
{
    let client = channel_tree__get_client_by_client_id(client_id);
    return client != null ? client.username : (typeof fallback_username === "string" ? fallback_username : "");
}

/**
 * @brief public key string for a client id
 *
 * @param number client_id -> the client id
 *
 * @return string|undefined the public key, undefined when the client is not in g_client_list
 */
function channel_tree__get_public_key_by_client_id(client_id)
{
    let result = "";
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i].client_id == client_id)
        {
            result = g_client_list[i].public_key;
            return result;
        }
    }
}

/**
 * @brief finds a connected user by saved identity
 *
 * @param identity_hash identity to find
 *
 * @return object|null -> the connected client, or null when absent
 */
function channel_tree__get_client_by_identity_hash(identity_hash)
{
    if (typeof identity_hash !== "string" || identity_hash.length === 0)
    {
        return null;
    }
    return g_client_list.find(function(client)
    {
        return client != null && client.identity_hash === identity_hash;
    }) || null;
}

/**
 * @brief finds a stored user by saved identity
 *
 * @param identity_hash identity to find
 *
 * @return object|null -> the stored client, or null when absent
 */
function channel_tree__get_stored_client_by_identity_hash(identity_hash)
{
    if (typeof identity_hash !== "string" || identity_hash.length === 0)
    {
        return null;
    }
    return g_offline_client_list.find(function(client)
    {
        return client != null && client.identity_hash === identity_hash;
    }) || null;
}

// rows follow tree order: channel header, its clients, then its child subtrees

/**
 * @brief whether a channel is another channel itself or is anywhere below it
 *
 * @param number child_channel_id -> the channel to test
 * @param number ancestor_channel_id -> the candidate ancestor
 *
 * @return boolean true when ancestor_channel_id is child_channel_id or one of its ancestors
 */
function channel_tree__is_channel_in_subtree_of(child_channel_id, ancestor_channel_id)
{
    let current = channel_tree__get_channel_by_id(g_channel_list, child_channel_id);
    let guard = 0;

    while (current != null && guard < 10000)
    {
        if (current.channel_id == ancestor_channel_id)
        {
            return true;
        }
        current = channel_tree__get_channel_by_id(g_channel_list, current.parent_channel_id);
        guard++;
    }

    return false;
}

/**
 * @brief finds the final rendered row in a channel's subtree
 * insert a new child channel after this row
 *
 * @param number channel_id -> the channel
 *
 * @return Element|null the last element, the channel header itself when the subtree is empty, null when the channel is not rendered
 */
function channel_tree__get_channel_subtree_last_element(channel_id)
{
    let header = document.querySelector('[data-channel-id="' + channel_id + '"]');
    let anchor = null;
    let node = null;

    if (header == null)
    {
        return null;
    }

    anchor = header;
    node = header.nextElementSibling;

    while (node != null)
    {
        let belongs = false;

        if (node.classList.contains("connected-client"))
        {
            let client = channel_tree__get_client_by_client_id(parseInt(node.getAttribute("data-connected-client-id")));
            if (client != null && channel_tree__is_channel_in_subtree_of(client.channel_id, channel_id))
            {
                belongs = true;
            }
        }
        else if (node.classList.contains("single-channel"))
        {
            if (channel_tree__is_channel_in_subtree_of(parseInt(node.getAttribute("data-channel-id")), channel_id))
            {
                belongs = true;
            }
        }

        if (belongs == false)
        {
            break;
        }

        anchor = node;
        node = node.nextElementSibling;
    }

    return anchor;
}

/**
 * @brief finds the final client row directly under a channel
 * insert a joining client after this row
 *
 * @param number channel_id -> the channel
 *
 * @return Element|null the last row, the header itself when the channel has no clients, null when the channel is not rendered
 */
function channel_tree__get_channel_own_clients_last_element(channel_id)
{
    let header = document.querySelector('[data-channel-id="' + channel_id + '"]');
    let anchor = null;
    let node = null;

    if (header == null)
    {
        return null;
    }

    anchor = header;
    node = header.nextElementSibling;

    while (node != null && node.classList.contains("connected-client"))
    {
        anchor = node;
        node = node.nextElementSibling;
    }

    return anchor;
}

/**
 * @brief builds a client row with its name, audio state, avatar and channel indentation
 *
 * @param object client -> raw server entry or stored client record
 * @param boolean is_local_client -> true for the local user, whose name is editable
 *
 * @return string the row html
 */
function channel_tree__generate_html_for_single_client(client, is_local_client)
{
    let result = "";
    let client_id = client.client_id;
    let channel_id_of_client = client.channel_id;
    let username = chat__sanitize_string(client.username);
    let audio_state = client.audio_state;
    let country_iso_code = client.country_iso_code;
    let is_music_bot = client.is_music_bot;
    // restore the unread badge from client state when rebuilding its row
    let unread_count = (typeof client.unread_count === "number") ? client.unread_count : 0;

    let client_audio_state_html_class = "client-audio-state-completely-disabled";
    switch (audio_state)
    {
        case 1:
            client_audio_state_html_class = "client-audio-state-microphone-active";
            break;

        case 2:
            client_audio_state_html_class = "client-audio-state-microphone-enabled";
            break;

        case 3:
            client_audio_state_html_class = "client-audio-state-microphone-disabled";
            break;

        case 4:
            client_audio_state_html_class = "client-audio-state-completely-disabled";
            break;
    }

    let additional_client_type_html_class = "";
    if (is_music_bot == true)
    {
        additional_client_type_html_class = "connected-client-p-music-bot";
    }

    let indentation_level = 1;

    if (channel_id_of_client != -2) // the idle channel is outside the ordinary channel tree
    {
        indentation_level = channel_tree__get_indentation_level(channel_id_of_client, g_channel_list) + 1;
    }

    let is_channel_collapsed = "";
    if (channel_tree__is_channel_or_his_parent_collapsed(channel_id_of_client) == true)
    {
        is_channel_collapsed = "collapsed";
    }

    // expose the avatar through --client-avatar. themes choose whether to paint it
    let avatar_inline_style = "";
    if (g_server_policy.is_avatar_upload_allowed == true && typeof client.base64_avatar === "string" && client.base64_avatar.length > 0)
    {
        avatar_inline_style = " data-has-avatar=\"1\" style=\"--client-avatar: url(" + client.base64_avatar + ");\"";
    }

    if (is_local_client)
    {
        result = "<div class=\"connected-local-client connected-client " + is_channel_collapsed + "\" data-connected-client-id=\"" + client_id + "\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                    </div>\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    </div>\n\
                    <input maxlength=\"40\" id=\"connected-local-client-input\" value=\"" + username + "\" style=\"--own-name-length: " + username.length + ";\">\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <span class=\"client-video-stream-icon\" data-client-id=\"" + client_id + "\" title=\"streaming - click to open the stream window\" style=\"display: none;\"></span>\n\
                    <span class=\"client-video-watching-icon\" data-client-id=\"" + client_id + "\" title=\"watching the stream\" style=\"display: none;\"></span>\n\
                    <div class='client-groups' id =\"client-groups-" + client_id + "\"></div>\n\
                </div>";
    }
    else
    {

        let client_mute_state_display = client.is_muted_by_local_client == true ? "block" : "none";
        let client_ignore_state_display = client.is_ignored_by_local_client == true ? "block" : "none";
        let is_idle_client_html = client.is_idle == true ? " idle-client" : "";
        let music_bot_client_html = client.is_music_bot == true ? " music-bot-client" : "";

        result = "<div class=\"connected-client " + is_channel_collapsed + " " + music_bot_client_html + " " + is_idle_client_html + "\" data-connected-client-id=\"" + client_id + "\" draggable=\"true\" style=\"--client-indent: " + indentation_level * 20 + "px;\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                    </div>\n\
                    " + chat__generate_unread_badge_html(unread_count) + "\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    <div class=\"client-mute-state\" title=\"Muted by you\" style=\"display: "+client_mute_state_display+";\"></div>\n\
                    </div>\n\
                    <p class=\"connected-client-p "+additional_client_type_html_class+"\"><span class=\"client-name-text\">" + username + "<svg class=\"client-ignore-state\" aria-hidden=\"true\" style=\"display: "+client_ignore_state_display+";\" viewBox=\"0 0 100 20\" preserveAspectRatio=\"none\"><path d=\"M1 2L99 18M1 18L99 2\"></path></svg></span></p>\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <span class=\"client-video-stream-icon\" data-client-id=\"" + client_id + "\" title=\"streaming - click to open the stream window\" style=\"display: none;\"></span>\n\
                    <span class=\"client-video-watching-icon\" data-client-id=\"" + client_id + "\" title=\"watching the stream\" style=\"display: none;\"></span>\n\
                    <div class='client-groups' id=\"client-groups-" + client_id + "\"></div>\n\
                </div>";
    }

    return result;
}

/**
 * @brief whether the channel itself or any of its ancestors is collapsed (recursive walk up)
 *
 * @param number channel_id -> the channel
 *
 * @return boolean true when the channel is hidden by a collapse
 */
function channel_tree__is_channel_or_his_parent_collapsed(channel_id)
{
    let result = false;
    let channel_index = channel_tree__get_channel_index_in_array_by_channel_id(g_channel_list, channel_id);

    if (channel_index == -1)
    {
        return false;
    }

    if (g_channel_list[channel_index].is_channel_directly_collapsed == false)
    {
        result = channel_tree__is_channel_or_his_parent_collapsed(g_channel_list[channel_index].parent_channel_id);
    }
    else
    {
        result = true;
    }
    return result;
}

/**
 * @brief ids of every channel below the given one (fully recursive), used when collapsing
 *
 * @param number channel_to_find -> the channel whose subtree is collapsed
 *
 * @return array the channel ids below it
 */
function channel_tree__find_subchannels_of_channel_for_collapse(channel_to_find)
{
    let result = [];

    for (var i = 0; i < g_channel_list.length; i++)
    {
        if (g_channel_list[i].parent_channel_id == channel_to_find)
        {
            result.push(g_channel_list[i].channel_id);
            let result1 = channel_tree__find_subchannels_of_channel_for_collapse(g_channel_list[i].channel_id);
            result = result.concat(result1);
        }
    }

    return result;
}

/**
 * @brief ids of the channels below the given one, without descending into subchannels that are themselves directly collapsed (those stay folded when expanding)
 *
 * @param number channel_to_find -> the channel being expanded
 *
 * @return array the channel ids to show
 */
function channel_tree__find_subchannels_of_channel_to_expand(channel_to_find)
{
    let result = [];

    for (var i = 0; i < g_channel_list.length; i++)
    {
        if (g_channel_list[i].parent_channel_id == channel_to_find)
        {
            result.push(g_channel_list[i].channel_id);

            if (g_channel_list[i].is_channel_directly_collapsed == true)
            {
                console.log("skipping channel" + g_channel_list[i].name);
                continue;
            }
            else
            {
                console.log("trying to find subchannels of channel " + g_channel_list[i].name);
                let result1 = channel_tree__find_subchannels_of_channel_to_expand(g_channel_list[i].channel_id);
                result = result.concat(result1);
            }
        }
    }

    return result;
}

/**
 * @brief client ids of everybody sitting in any of the given channels
 *
 * @param array channel_ids -> the channel ids
 *
 * @return array the client ids
 */
function channel_tree__find_clients_in_channel(channel_ids)
{
    let result = [];

    for (var i = 0; i < g_client_list.length; i++)
    {
        if (channel_ids.includes(g_client_list[i].channel_id))
        {
            result.push(g_client_list[i].client_id);
        }
    }
    return result;
}

// avatar loading and rendering

/**
 * @brief updates a client's avatar in its cached state and visible controls
 *
 * @param number client_id -> the client
 * @param string|null base64 -> the avatar as a data url, empty or null to clear it
 *
 * @return void
 */
function channel_tree__apply_avatar_to_ui(client_id, base64)
{
    if (client_id === undefined || client_id === null)
    {
        return;
    }

    let has_avatar = (typeof base64 === "string" && base64.length > 0);

    // cache the avatar on the client record so later row renders retain it
    let client_object = channel_tree__get_client_by_client_id(client_id);
    if (client_object != null)
    {
        client_object.base64_avatar = has_avatar ? base64 : null;
    }

    android_host__refresh_message_avatars(client_id);

    // update the row marker and CSS variable without waiting for a full render
    let tree_circle = document.getElementById("client-audio-state-" + client_id);
    if (tree_circle != null)
    {
        if (has_avatar == true && g_server_policy.is_avatar_upload_allowed == true)
        {
            tree_circle.style.setProperty("--client-avatar", "url(" + base64 + ")");
            tree_circle.setAttribute("data-has-avatar", "1");
        }
        else
        {
            tree_circle.style.removeProperty("--client-avatar");
            tree_circle.removeAttribute("data-has-avatar");
        }
    }

    // update the people counts
    channel_tree__refresh_people_counts();

    // the member list is a clone of the tree, so repaint it too
    if (g_avatar.grid_visible == true)
    {
        UI.schedule_member_list_sync();
    }

    // update the profile picture only for the selected client
    if (parseInt(g_profile_avatar_client_id) === parseInt(client_id))
    {
        let big = document.getElementById("current-client-avatar");
        if (big != null)
        {
            if (has_avatar)
            {
                big.style.backgroundImage = "url(" + base64 + ")";
                big.style.backgroundSize = "100% 100%";

                // a picture replaces the letter that was standing in for it
                big.textContent = "";
                big.classList.remove("avatar-empty");
            }
            else
            {
                big.style.backgroundImage = "";
            }
        }
    }
}

/**
 * @brief the first letter, shown in the profile avatar circle while there is no picture in it
 *
 * @param string username -> the name the letter is taken from
 *
 * @return void
 */
function channel_tree__set_profile_avatar_monogram(username)
{
    let big = document.getElementById("current-client-avatar");

    if (big == null)
    {
        return;
    }

    big.textContent = (typeof username === "string" && username.length > 0) ? username.charAt(0) : "?";
    big.classList.add("avatar-empty");
}

/**
 * @brief the profile line under the name, built only from things the client list actually knows: you, music bot, channel, idle, country, muted, ignored
 *
 * @param object client -> the client from g_client_list
 *
 * @return string the parts joined with " · "
 */
function channel_tree__describe_client_for_profile(client)
{
    let parts = [];

    if (client.client_id === g_local_client_id)
    {
        parts.push("you");
    }
    if (client.is_music_bot == true)
    {
        parts.push("music bot");
    }

    let channel = channel_tree__get_channel_by_id(g_channel_list, client.channel_id);

    if (channel != null)
    {
        parts.push("in " + channel.name);
    }

    if (client.is_idle == true)
    {
        parts.push("idle");
    }
    if (client.country_iso_code != null && client.country_iso_code.length > 0)
    {
        parts.push(client.country_iso_code.toUpperCase());
    }
    if (client.is_muted_by_local_client == true)
    {
        parts.push("muted by you");
    }
    if (client.is_ignored_by_local_client == true)
    {
        parts.push("ignored by you");
    }

    return parts.join(" · ");
}

/**
 * @brief requests one client's avatar when avatars are enabled
 *
 * @param number client_id -> the client
 *
 * @return void
 */
function channel_tree__request_single_avatar(client_id)
{
    if (g_server_policy.is_avatar_upload_allowed == false || client_id === undefined || client_id === null)
    {
        return;
    }
    let message_object = { message: { type: "request_avatar_for_client", client_id: parseInt(client_id) } };
    connection__send_message_object(message_object);
}

/**
 * @brief queues connected clients for the avatar grid's chunked loading
 *
 * @return void
 */
function channel_tree__enqueue_all_avatars_for_loading()
{
    // the avatar grid requests all visible client avatars in chunks
    if (g_server_policy.is_avatar_upload_allowed == false || g_avatar.grid_visible == false)
    {
        return;
    }
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null)
        {
            g_avatar.load_queue.push(g_client_list[i].client_id);
        }
    }
    if (g_avatar.load_scheduled == false)
    {
        channel_tree__pump_avatar_load_queue(50);
    }
}

/**
 * @brief queues missing avatars and starts the background prefetch timer
 *
 * @return void
 */
function channel_tree__start_avatar_prefetch()
{
    if (PREFETCH_AVATARS_AUTOMATICALLY == false || g_server_policy.is_avatar_upload_allowed == false)
    {
        return;
    }

    // the avatar grid already queues its clients. avoid duplicate prefetch requests
    if (g_avatar.grid_visible == true)
    {
        return;
    }

    for (let i = 0; i < g_client_list.length; i++)
    {
        let client_object = g_client_list[i];

        if (client_object == null)
        {
            continue;
        }
        if (client_object.client_id == g_local_client_id && g_show_message_avatars == false)
        {
            continue;
        }
        if (client_object.is_music_bot == true)
        {
            continue;
        }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0)
        {
            continue;
        }
        if (g_avatar.prefetch_queue.indexOf(client_object.client_id) != -1)
        {
            continue;
        }

        g_avatar.prefetch_queue.push(client_object.client_id);
    }

    if (g_avatar.prefetch_timer != null || g_avatar.prefetch_queue.length == 0)
    {
        return;
    }

    // one request per tick, so a room full of people never arrives as a burst
    g_avatar.prefetch_timer = setInterval(function()
    {
        if (g_server_policy.is_avatar_upload_allowed == false || g_avatar.prefetch_queue.length == 0)
        {
            channel_tree__stop_avatar_prefetch();
            return;
        }

        let client_id = g_avatar.prefetch_queue.shift();
        let client_object = channel_tree__get_client_by_client_id(client_id);

        // they left, or their avatar arrived some other way while waiting in the queue
        if (client_object == null)
        {
            return;
        }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0)
        {
            return;
        }

        channel_tree__request_single_avatar(client_id);
    }, AVATAR_PREFETCH_INTERVAL_MS);
}

/**
 * @brief stops the prefetch drain timer and empties g_avatar.prefetch_queue
 *
 * @return void
 */
function channel_tree__stop_avatar_prefetch()
{
    if (g_avatar.prefetch_timer != null)
    {
        clearInterval(g_avatar.prefetch_timer);
        g_avatar.prefetch_timer = null;
    }

    g_avatar.prefetch_queue.length = 0;
}

/**
 * @brief requests queued avatars in growing batches until the queue is empty
 * wait 400 ms between batches and cap each batch at 150 clients
 *
 * @param number chunk_size -> how many avatars this round asks for
 *
 * @return void
 */
function channel_tree__pump_avatar_load_queue(chunk_size)
{
    if (g_server_policy.is_avatar_upload_allowed == false || g_avatar.load_queue.length === 0)
    {
        g_avatar.load_scheduled = false;
        return;
    }

    let ids = g_avatar.load_queue.splice(0, chunk_size);
    let message_object = { message: { type: "request_avatars", client_ids: ids } };
    connection__send_message_object(message_object);

    if (g_avatar.load_queue.length > 0)
    {
        g_avatar.load_scheduled = true;
        let next_chunk = Math.min(chunk_size + 50, 150);
        setTimeout(function()
        {
            channel_tree__pump_avatar_load_queue(next_chunk);
        }, 400);
    }
    else
    {
        g_avatar.load_scheduled = false;
    }
}

/**
 * @brief updates channel counts and avatar previews for visible people
 *
 * @return void
 */
function channel_tree__refresh_people_counts()
{
    let people_by_channel = {};

    // only with a page
    if (!G_HAS_DOM)
    {
        return;
    }

    // collect the people
    for (let client of g_client_list)
    {
        if (client.is_music_bot == true || client.is_idle == true)
        {
            continue;
        }
        if (people_by_channel[client.channel_id] == null)
        {
            people_by_channel[client.channel_id] = [];
        }
        people_by_channel[client.channel_id].push(client);
    }

    // update each row
    for (let row of document.querySelectorAll("#channel-list-container .single-channel"))
    {
        let people = people_by_channel[row.getAttribute("data-channel-id")] || [];
        let element = row.querySelector(":scope > .single-channel-people");
        let shown = people.slice(0, 3);
        let state = people.length + ":" + shown.map(function(client)
        {
            return client.client_id + "/" + (client.base64_avatar || "").length;
        }).join(",");
        let count = null;

        // nobody inside
        if (people.length == 0)
        {
            if (element != null)
            {
                element.remove();
            }
            continue;
        }

        // nothing changed
        if (element != null && element.getAttribute("data-people") === state)
        {
            continue;
        }

        // create the element
        if (element == null)
        {
            element = document.createElement("div");
            element.className = "single-channel-people";
            row.appendChild(element);
        }

        // up to three pictures
        element.textContent = "";
        for (let client of shown)
        {
            let picture = document.createElement("span");
            picture.className = "single-channel-people-picture";
            if (client.base64_avatar && g_server_policy.is_avatar_upload_allowed == true)
            {
                picture.style.backgroundImage = "url(" + client.base64_avatar + ")";
            }
            else
            {
                picture.textContent = String(client.username || "").charAt(0);
            }
            element.appendChild(picture);
        }

        // the number
        count = document.createElement("span");
        count.className = "single-channel-people-count";
        count.textContent = people.length;
        element.appendChild(count);
        element.setAttribute("data-people", state);
        element.setAttribute("aria-label", people.length == 1 ? "1 person in this channel" : people.length + " people in this channel");
    }
}

/**
 * @brief restores and wires local channel icon and people count preferences
 *
 * @return void
 */
function channel_tree__setup_icon_preference()
{
    // only with a page
    if (!G_HAS_DOM)
    {
        return;
    }

    // each setting
    for (let [word, name] of [["hide", "server-group-icons"], ["hide", "channel-group-icons"], ["hide", "channel-icons"], ["show", "channel-people"]])
    {
        let checkbox = document.getElementById("local-settings-" + word + "-" + name);
        let storage_key = "lemon_" + word + "_" + name.replaceAll("-", "_");
        let body_class = word + "-" + name + "-in-channel-list";

        // restore saved value
        checkbox.checked = utils__storage_get(storage_key) === "1";
        document.body.classList.toggle(body_class, checkbox.checked);

        // save on change
        checkbox.onchange = function()
        {
            utils__storage_set(storage_key, checkbox.checked ? "1" : "0");
            document.body.classList.toggle(body_class, checkbox.checked);
        };
    }

    // flag setting
    channel_tree__setup_flag_preference();
}

/**
 * @brief applies country flag visibility to existing and future rows
 *
 * @param boolean is_saved -> true also writes the choice to localStorage
 *
 * @return void
 */
function channel_tree__apply_flag_preference(is_saved)
{
    document.body.classList.toggle("hide-country-flags", g_show_hide_toggle == true);
    if (is_saved == true)
    {
        utils__storage_set("lemon_show_flags", g_show_hide_toggle == true ? "0" : "1");
    }
}

/**
 * @brief restores the country flag preference and connects its controls
 *
 * @return void
 */
function channel_tree__setup_flag_preference()
{
    let checkbox = document.getElementById("local-settings-country-flags");
    let button = document.getElementById("hide-show-flags-button");
    /**
     * @brief defers flag refresh until the visibility event handlers have finished
     *
     * @return void
     */
    let follow_change = function()
    {
        // the timer runs after every handler of the event, so g_show_hide_toggle already has its new value
        setTimeout(function()
        {
            channel_tree__apply_flag_preference(true);
        }, 0);
    };

    // restore saved setting
    g_show_hide_toggle = utils__storage_get("lemon_show_flags") === "0";
    channel_tree__apply_flag_preference(false);
    if (checkbox != null)
    {
        checkbox.checked = (g_show_hide_toggle == false);
    }

    // add listeners once
    if (is_flag_preference_listener_added == true)
    {
        return;
    }
    is_flag_preference_listener_added = true;

    // connect the checkbox
    if (checkbox != null)
    {
        checkbox.addEventListener("change", follow_change);
    }

    // connect the button
    if (button != null)
    {
        button.addEventListener("click", follow_change);
    }
}
