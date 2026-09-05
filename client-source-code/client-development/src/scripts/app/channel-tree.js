// channel-tree.js is embedded in template.html along with the other client files, and in the node bundle
// it holds the lookups over g_channel_list, g_client_list, g_icons and g_tags, the html of a client row,
// and the avatars on those rows (the cache, the lazy loading queue and the prefetch after joining)
// messages.js and ui.js call it while rendering the tree

// state private to this file
var PREFETCH_AVATARS_AUTOMATICALLY = true;

var AVATAR_PREFETCH_INTERVAL_MS = 300;

// ---- lookups and the client row ----

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
 * @brief tag object from g_tags by id
 *
 * @param number tag_id -> the tag id
 *
 * @return object|null the tag, null when not found
 */
function channel_tree__get_tag_by_tag_id(tag_id)
{
    for (var i = 0; i < g_tags.length; i++)
    {
        if (g_tags[i].tag_id == tag_id)
        {
            return g_tags[i];
        }
    }
    return null;
}

/**
 * @brief the direct children of a parent channel
 *        pass g_ROOT_LEVEL_PARENT_SENTINEL to get the root channels (those are matched by their
 *        is_root_channel flag, not by parent id)
 *
 * @param array channels -> the channel list to search
 * @param number parent_channel_id -> the parent, or the sentinel for the roots
 *
 * @return array the child channel objects
 */
function channel_tree__get_channels_by_channel_parent_id(channels, parent_channel_id)
{
    let result = [];
    for (var i = 0; i < channels.length; i++)
    {
        if (parent_channel_id == g_ROOT_LEVEL_PARENT_SENTINEL)
        {
            // root channels no longer carry a usable parent id (uint64 has no -1),
            // so match them by the explicit is_root_channel flag sent by the server
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
 * @brief depth of a channel in the tree, walked up the parent ids
 *
 * @param number channel_id -> the channel to measure
 * @param array channels -> the channel list to walk
 *
 * @return number 0 for a root; the depth reached so far when the parent chain is broken or cyclic (logged as an error)
 */
function channel_tree__get_indentation_level(channel_id, channels)
{
    let result = 0;
    let is_root_channel_found = false;
    let channel_id_currently_looking_for = channel_id;

    let safety_counter = 0;

    while (is_root_channel_found == false)
    {
        // a valid parent chain reaches a root in at most channels.length steps. if it does not,
        // the data is malformed (missing root or a cycle) - bail out instead of looping forever
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

        // the channel we are looking for is not in the list - the chain is broken, stop
        if (was_channel_found == false)
        {
            console.error("channel_tree__get_indentation_level: channel_id " + channel_id_currently_looking_for + " not found while walking parents - channel data looks invalid");
            return result;
        }
    }
    return result;
}

/**
 * @brief whether the local client carries the admin tag (tag id 0)
 *
 * @return boolean true for an admin
 */
function channel_tree__is_local_client_admin()
{
    let result = false;
    let client = channel_tree__get_client_by_client_id(g_local_client_id);
    if (client.tag_ids.includes(0))
    {
        result = true;
    }
    return result;
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
 * @brief index of a client in g_client_list via the id -> index map
 *        a map hands back undefined for a miss, which no -1 check catches, so the miss is
 *        turned into -1 here (and logged, since the map and the list then disagree)
 *
 * @param number client_id -> the client id, must be a number
 *
 * @return number the index, -1 when the id is not there
 */
function channel_tree__get_client_index_in_array_by_client_id(client_id)
{
    let index = g_map_client_id_to_array_index.get(client_id);

    // callers all check for -1, and a map hands back undefined instead. undefined == -1
    // is false, so the check passed and the array lookup threw one line later
    if (index === undefined)
    {
        // the map and the client list disagree. the list length says which way:
        // a non-empty list here means the two were cleared at different moments
        console.warn("client lookup miss: no entry for client_id " + client_id
            + ", client list holds " + g_client_list.length);

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
 * @brief the name a person is labelled with: the admin-granted alias when there is one, else the username
 *        for anything the user reads, while channel_tree__get_username_by_client_id serves the protocol
 *
 * @param number client_id -> the client id
 * @param string fallback_username -> used when the client is not in the list
 *
 * @return string the alias, the username, the fallback, else ""
 */
function channel_tree__get_display_name_by_client_id(client_id, fallback_username)
{
    let client_object = channel_tree__get_client_by_client_id(client_id);

    if (client_object != null && typeof client_object.alias === "string" && client_object.alias.length > 0)
    {
        return client_object.alias;
    }

    if (client_object != null && typeof client_object.username === "string" && client_object.username.length > 0)
    {
        return client_object.username;
    }

    return (typeof fallback_username === "string") ? fallback_username : "";
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
 * @brief a CONNECTED client carrying this alias
 *        aliases are unique per identity, so this is how an offline conversation finds its owner
 *        once they come back
 *
 * @param string alias -> the alias, compared case-insensitively
 *
 * @return object|null the client, null when nobody connected carries it
 */
function channel_tree__get_client_by_alias(alias)
{
    if (typeof alias !== "string" || alias.length == 0) { return null; }

    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null && typeof g_client_list[i].alias === "string" && g_client_list[i].alias.toLowerCase() == alias.toLowerCase())
        {
            return g_client_list[i];
        }
    }

    return null;
}

/**
 * @brief offline contact from g_offline_client_list by alias
 *
 * @param string alias -> the alias, compared case-insensitively
 *
 * @return object|null the stored client, null when not found
 */
function channel_tree__get_stored_client_by_alias(alias)
{
    if (typeof alias !== "string" || alias.length == 0) { return null; }

    for (let i = 0; i < g_offline_client_list.length; i++)
    {
        if (g_offline_client_list[i].alias.toLowerCase() == alias.toLowerCase())
        {
            return g_offline_client_list[i];
        }
    }

    return null;
}

// tree ordering helpers: the channel list is a flat pre-order list (a channel header, its client rows,
// then its subchannels' subtrees), so new items are inserted relative to these anchors

/**
 * @brief whether a channel is another channel itself or sits anywhere below it
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
 * @brief the last DOM element belonging to a channel's whole subtree (its clients, its subchannels and their content)
 *        insert a new subchannel after this
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
 * @brief the last of a channel's own .connected-client rows (the contiguous run right after its header, before any subchannel)
 *        insert a joining client after this
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
 * @brief the channel-tree row of one client: mic-state circle, name (an editable input for the local client), alias, flag, song marquee and tags, indented by channel depth
 *
 * @param object client -> the client from g_client_list
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
    let alias_text = chat__sanitize_string((client.alias != null) ? client.alias : "");
    let has_alias_html_class = (alias_text.length > 0) ? " has-alias" : "";
    let audio_state = client.audio_state;
    let country_iso_code = client.country_iso_code;
    let is_music_bot = client.is_music_bot;
    // every theme hides this badge by default, so it needs the inline display to show at all.
    // painting it from state means a re-rendered row (channel join, idle) keeps its count
    // instead of silently resetting to 0, which is what the hardcoded markup used to do
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

    if (channel_id_of_client != -2) // if channel is idle channel, (ID -2), do not run this function, program will enter infinite loop
    {
        indentation_level = channel_tree__get_indentation_level(channel_id_of_client, g_channel_list) + 1;
    }

    let is_channel_collapsed = "";
    if (channel_tree__is_channel_or_his_parent_collapsed(channel_id_of_client) == true)
    {
        is_channel_collapsed = "collapsed";
    }

    // the avatar is offered to the row as a css variable plus a marker, not painted: in most themes
    // that circle is the mic-state icon; a theme that wants faces (termix) reads var(--client-avatar)
    let avatar_inline_style = "";
    if (g_server_policy.allow_avatars == true && typeof client.base64_avatar === "string" && client.base64_avatar.length > 0)
    {
        avatar_inline_style = " data-has-avatar=\"1\" style=\"--client-avatar: url(" + client.base64_avatar + ");\"";
    }

    if (is_local_client)
    {
        result = "<div class=\"connected-local-client connected-client " + is_channel_collapsed + has_alias_html_class + "\" data-connected-client-id=\"" + client_id + "\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                    </div>\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    </div>\n\
                    <input maxlength=\"40\" id=\"connected-local-client-input\" value=\"" + username + "\">\n\
                    <p class=\"client-alias\">" + alias_text + "</p>\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <div class='client-tags' id =\"client-tags-" + client_id + "\">\n\
                    </div>\n\
                </div>";
    }
    else
    {

        let client_mute_state_display = client.is_muted_by_local_client == true ? "block" : "none";
        let client_ignore_state_display = client.is_ignored_by_local_client == true ? "block" : "none";
        let is_idle_client_html = client.is_idle == true ? " idle-client" : "";
        let music_bot_client_html = client.is_music_bot == true ? " music-bot-client" : "";

        result = "<div class=\"connected-client " + is_channel_collapsed + " " + music_bot_client_html + " " + is_idle_client_html + has_alias_html_class + "\" data-connected-client-id=\"" + client_id + "\">\n\
                    <div style=\"width: " + indentation_level * 20 + "px; height: 1px; vertical-align:top; display: inline-block;\">\n\
                        <div class=\"client-mute-state\" style=\"display: "+client_mute_state_display+";\"></div>\n\
                    </div>\n\
                    <div class='client-audio-state "+ client_audio_state_html_class + "' id=\"client-audio-state-" + client_id + "\"" + avatar_inline_style + "><div class=\"client-audio-availability\"></div>\n\
                    </div>\n\
                    <p class=\"connected-client-p "+additional_client_type_html_class+"\">" + username + "</p>\n\
                    <p class=\"client-alias\">" + alias_text + "</p>\n\
                    <div class=\"connected-client-country-flag country-flag-"+(country_iso_code ? (""+country_iso_code).toLowerCase() : "")+"\"></div>\n\
                    <p class=\"connected-client-p-received-messages-number\" style=\"display: "+((unread_count > 0) ? "inline-block" : "none")+";\">"+unread_count+"</p>\n\
                    <div class=\"marquee-music-playing-container\" data-marquee-music-playing-container-id='"+ client_id + "'\>\n\
                        <p class='p-marquee-song-icon'>[</p>\n\
                        <div class=\"marquee\">\n\
                            <div id='marquee-song-name-client-id-"+ client_id + "'></div>\n\
                        </div>\n\
                        <p>]</p>\n\
                    </div>\n\
                    <div class='client-tags' id=\"client-tags-" + client_id + "\">\n\
                    </div>\n\
                    <svg class=\"client-ignore-state\" style=\"margin-left: "+indentation_level * 20+"px; display: "+client_ignore_state_display+";\"  viewBox=\"0 0 100 20\">\n\
                        <line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"20\"></line>\n\
                        <line x1=\"0\" y1=\"20\" x2=\"100\" y2=\"0\"></line>\n\
                    </svg>\n\
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

// ---- avatars ----

/**
 * @brief puts a client's avatar on everything that shows it: the client object, the tree row, the member list and the profile pane
 *        a variable plus a marker on the row, never a painted background, so a new or cleared
 *        avatar shows without waiting for the next full render
 *
 * @param number client_id -> the client
 * @param string|null base64 -> the avatar as a data url, empty or null to clear it
 *
 * @return void
 */
function channel_tree__apply_avatar_to_ui(client_id, base64)
{
    if (client_id === undefined || client_id === null) { return; }

    let has_avatar = (typeof base64 === "string" && base64.length > 0);

    // keep the avatar ON the client object in g_client_list, so it travels with the client and is
    // re-applied by avatar_inline_style_for_client on every tree render (channel switch etc.)
    let client_object = channel_tree__get_client_by_client_id(client_id);
    if (client_object != null) { client_object.base64_avatar = has_avatar ? base64 : null; }

    // offer it to the tree row the way the renderer does (a variable plus a marker, never a painted
    // background), so a new or cleared avatar shows without waiting for the next full render
    let tree_circle = document.getElementById("client-audio-state-" + client_id);
    if (tree_circle != null)
    {
        if (has_avatar == true && g_server_policy.allow_avatars == true)
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

    // the member list is a clone of the tree, so repaint it too
    if (g_avatar.grid_visible == true) { UI.schedule_member_list_sync(); }

    // the big avatar in the right-pane profile, only if this is the client shown there
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

    if (big == null) { return; }

    big.textContent = (typeof username === "string" && username.length > 0) ? username.charAt(0) : "?";
    big.classList.add("avatar-empty");
}

/**
 * @brief the profile line under the name, built only from things the client list actually knows: you, music bot, channel, idle, alias, country, muted, ignored
 *
 * @param object client -> the client from g_client_list
 *
 * @return string the parts joined with " · "
 */
function channel_tree__describe_client_for_profile(client)
{
    let parts = [];

    if (client.client_id === g_local_client_id) { parts.push("you"); }
    if (client.is_music_bot == true) { parts.push("music bot"); }

    let channel = channel_tree__get_channel_by_id(g_channel_list, client.channel_id);

    if (channel != null) { parts.push("in " + channel.name); }

    if (client.is_idle == true) { parts.push("idle"); }
    if (client.alias != null && client.alias.length > 0) { parts.push("known as " + client.alias); }
    if (client.country_iso_code != null && client.country_iso_code.length > 0) { parts.push(client.country_iso_code.toUpperCase()); }
    if (client.is_muted_by_local_client == true) { parts.push("muted by you"); }
    if (client.is_ignored_by_local_client == true) { parts.push("ignored by you"); }

    return parts.join(" · ");
}

/**
 * @brief asks the server for one client's avatar; no-op when avatars are disabled
 *
 * @param number client_id -> the client
 *
 * @return void
 */
function channel_tree__request_single_avatar(client_id)
{
    if (g_server_policy.allow_avatars == false || client_id === undefined || client_id === null) { return; }
    let message_object = { message: { type: "request_avatar_for_client", client_id: parseInt(client_id) } };
    connection__send_message_object(message_object);
}

/**
 * @brief chunked lazy load for the avatar grid: enqueues every connected client id and pulls avatars in growing chunks (50, then 100, then 150)
 *        only the grid member list shows everybody at once; other themes fetch the clicked client's avatar alone
 *
 * @return void
 */
function channel_tree__enqueue_all_avatars_for_loading()
{
    // only bulk-load everyone's avatar for the avatar grid member list (shown all at once).
    // other themes only ever fetch the clicked client's avatar for the big right-pane.
    if (g_server_policy.allow_avatars == false || g_avatar.grid_visible == false) { return; }
    for (let i = 0; i < g_client_list.length; i++)
    {
        if (g_client_list[i] != null) { g_avatar.load_queue.push(g_client_list[i].client_id); }
    }
    if (g_avatar.load_scheduled == false) { channel_tree__pump_avatar_load_queue(50); }
}

/**
 * @brief queues every connected person whose avatar we do not have yet and drains the queue one request per tick
 *        safe to call repeatedly: already-queued and already-known ids are skipped, the timer is
 *        started once; the strip themes bulk-load through channel_tree__enqueue_all_avatars_for_loading instead
 *
 * @return void
 */
function channel_tree__start_avatar_prefetch()
{
    if (PREFETCH_AVATARS_AUTOMATICALLY == false || g_server_policy.allow_avatars == false) { return; }

    // the strip themes (simpledark/bluebell) show everybody at once and already bulk-load
    // through channel_tree__enqueue_all_avatars_for_loading; prefetching on top would double the requests
    if (g_avatar.grid_visible == true) { return; }

    for (let i = 0; i < g_client_list.length; i++)
    {
        let client_object = g_client_list[i];

        if (client_object == null) { continue; }
        if (client_object.client_id == g_local_client_id) { continue; }
        if (client_object.is_music_bot == true) { continue; }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0) { continue; }
        if (g_avatar.prefetch_queue.indexOf(client_object.client_id) != -1) { continue; }

        g_avatar.prefetch_queue.push(client_object.client_id);
    }

    if (g_avatar.prefetch_timer != null || g_avatar.prefetch_queue.length == 0) { return; }

    // one request per tick, so a room full of people never arrives as a burst
    g_avatar.prefetch_timer = setInterval(function()
    {
        if (g_server_policy.allow_avatars == false || g_avatar.prefetch_queue.length == 0)
        {
            channel_tree__stop_avatar_prefetch();
            return;
        }

        let client_id = g_avatar.prefetch_queue.shift();
        let client_object = channel_tree__get_client_by_client_id(client_id);

        // they left, or their avatar arrived some other way while waiting in the queue
        if (client_object == null) { return; }
        if (typeof client_object.base64_avatar === "string" && client_object.base64_avatar.length > 0) { return; }

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
 * @brief drains g_avatar.load_queue: requests one chunk of avatars now, then re-schedules itself every 400 ms with a growing chunk size (max 150) until the queue is empty
 *
 * @param number chunk_size -> how many avatars this round asks for
 *
 * @return void
 */
function channel_tree__pump_avatar_load_queue(chunk_size)
{
    if (g_server_policy.allow_avatars == false || g_avatar.load_queue.length === 0)
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
        setTimeout(function() { channel_tree__pump_avatar_load_queue(next_chunk); }, 400);
    }
    else
    {
        g_avatar.load_scheduled = false;
    }
}
