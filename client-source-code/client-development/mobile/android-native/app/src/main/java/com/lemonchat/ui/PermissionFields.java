package com.lemonchat.ui;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** permission labels and categories shared with the browser client */
final class PermissionFields
{
    static final class Field
    {
        final String name;
        final String label;
        final String help;
        final String category;
        final boolean power;
        final boolean skip;
        final boolean defaultAllowed;
        final boolean channel;

        Field(String name, String label, String help, String category, boolean power, boolean skip, boolean defaultAllowed, boolean channel)
        {
            this.name = name;
            this.label = label;
            this.help = help;
            this.category = category;
            this.power = power;
            this.skip = skip;
            this.defaultAllowed = defaultAllowed;
            this.channel = channel;
        }

        boolean matches(String query)
        {
            String needle = query.trim().toLowerCase(Locale.ROOT);
            return (name + " " + label + " " + help).toLowerCase(Locale.ROOT).contains(needle);
        }
    }

    static final List<String> CATEGORIES = Collections.unmodifiableList(Arrays.asList("Channels", "Moderation", "Groups", "Messaging", "Music bots", "Icons and emojis", "Channel group management", "Information and access"));
    static final List<String> CHANNEL_CATEGORIES = Collections.unmodifiableList(Arrays.asList("Channel actions", "Channel powers"));

    static final List<Field> SERVER = Collections.unmodifiableList(Arrays.asList
    (
        new Field("create_channel", "Create permanent channels", "Enables permanent channel creation.", "Channels", false, false, false, false),
        new Field("create_temp_channel", "Create temporary channels", "Enables one temporary channel per user. Users with Create permanent channels make permanent channels instead.", "Channels", false, false, true, false),
        new Field("kick", "Kick users", "Kick power must reach the target's protection.", "Moderation", false, false, false, false),
        new Field("ban", "Ban users", "Ban power must reach the target's protection.", "Moderation", false, false, false, false),
        new Field("view_stored_users", "View saved users", "Shows saved users, including those who are offline. The server must enable this list.", "Information and access", false, false, false, false),
        new Field("send_offline_messages", "Send offline messages", "Lets this user leave encrypted messages for offline recipients. The server must enable offline messages.", "Messaging", false, false, false, false),
        new Field("receive_offline_messages", "Receive offline messages", "Lets others leave encrypted messages for this user while offline. The sender also needs permission.", "Messaging", false, false, false, false),
        new Field("message_idle_clients", "Message idle clients", "Lets this user send private messages to clients in idle mode. The server must allow messages to idle clients.", "Messaging", false, false, true, false),
        new Field("call_idle_clients", "Call idle clients", "Lets this user send a call notification to an idle client. The server must enable idle mode.", "Messaging", false, false, true, false),
        new Field("view_group_permissions", "View group permissions", "Shows the permission values of server groups. This does not allow editing them.", "Groups", false, false, false, false),
        new Field("view_group_members", "View group members", "Shows connected and stored members in the Groups window. This does not allow changing memberships.", "Groups", false, false, false, false),
        new Field("create_groups", "Create groups", "Creates groups from a preset within your permissions. Editing their permissions requires a separate right.", "Groups", false, false, false, false),
        new Field("edit_groups", "Edit group names and icons", "Changes a group name or its selected icon when management power reaches the group protection.", "Groups", false, false, false, false),
        new Field("delete_groups", "Delete groups", "Deletes groups within your management power. You cannot delete your own group, the built in Admin group or the default Guest group.", "Groups", false, false, false, false),
        new Field("assign_groups", "Assign groups", "Assigns groups within your management power. You cannot assign yourself or grant rights you do not hold.", "Groups", false, false, false, false),
        new Field("remove_groups", "Remove groups", "Removes groups within your management power. Removing the last group restores the default Guest group.", "Groups", false, false, false, false),
        new Field("edit_group_permissions", "Edit group permissions", "Changes permissions within your management power. You cannot edit your own group or grant rights and powers you do not hold.", "Groups", false, false, false, false),
        new Field("upload_icons", "Upload server icons", "Adds shared server icons. Selecting an icon for a group requires permission to edit that group.", "Icons and emojis", false, false, false, false),
        new Field("view_client_details", "View client details", "Shows identity, country and last activity. IP addresses and connection duration have separate permissions.", "Information and access", false, false, false, false),
        new Field("view_client_connection_time", "View connection duration", "Shows how long a client has been connected to this server. Enabled for guests by default.", "Information and access", false, false, true, false),
        new Field("view_server_logs", "View server logs", "Reads the server log. Clearing it and changing what is recorded still require admin access.", "Information and access", false, false, false, false),
        new Field("view_server_info", "View server information", "Shows information the server publishes, including its banner, uptime and used slots. Enabled by default.", "Information and access", false, false, true, false),
        new Field("delete_icons", "Delete server icons", "Deletes shared server icons. Does not grant upload rights.", "Icons and emojis", false, false, false, false),
        new Field("upload_emojis", "Upload server emojis", "Adds server reaction images, including supported GIF and WebP images.", "Icons and emojis", false, false, false, false),
        new Field("delete_emojis", "Delete server emojis", "Removes emojis from the server picker.", "Icons and emojis", false, false, false, false),
        new Field("view_ip_addresses", "View IP addresses", "Shows addresses in client details and the ban list. Viewing those lists needs its own permission.", "Information and access", false, false, false, false),
        new Field("view_bans", "View bans", "Reads the ban list without allowing new bans.", "Moderation", false, false, false, false),
        new Field("remove_bans", "Remove bans", "Removes existing bans. Does not grant the right to ban users.", "Moderation", false, false, false, false),
        new Field("move_clients", "Move users between channels", "Requires move power to reach the target protection. Channel rights must cover both channels.", "Moderation", false, false, false, false),
        new Field("kick_from_channel", "Kick users from a channel", "Moves users to root without disconnecting them. Requires move power.", "Moderation", false, true, false, false),
        new Field("mute_clients", "Mute users temporarily", "Mutes microphone and MP3 audio in the selected channel, subject to moderation protection.", "Moderation", false, true, false, false),
        new Field("stop_client_streams", "Stop video streams", "Ends an active screen share or video stream, subject to moderation protection.", "Moderation", false, true, false, false),
        new Field("edit_channel_name", "Edit channel names", "Changes the name without granting other channel edits.", "Channels", false, true, false, false),
        new Field("edit_channel_description", "Edit channel descriptions", "Changes the description without granting other channel edits.", "Channels", false, true, false, false),
        new Field("bypass_channel_password", "Bypass channel passwords", "Enter password-protected channels without a password. Channel groups grant this only for their assigned channel; capacity limits still apply.", "Channels", false, true, false, false),
        new Field("edit_channel_password", "Edit channel passwords", "Sets, replaces or removes a channel password.", "Channels", false, true, false, false),
        new Field("edit_channel_powers", "Edit channel power requirements", "Changes the required talk, message, picture and streaming powers.", "Channels", false, true, false, false),
        new Field("edit_channel_options", "Edit channel options and icons", "Changes audio, streaming, client limits and the selected channel icon.", "Channels", false, true, false, false),
        new Field("delete_channel", "Delete channels", "Deletes channels. Every child channel must also be within your permissions.", "Channels", false, true, false, false),
        new Field("assign_channel_groups", "Assign channel groups", "Assigns or removes a channel group within your management power. It applies only in that channel.", "Channels", false, true, false, false),
        new Field("send_direct_pictures", "Send direct pictures", "Enables picture uploads to individual users. Enabled by default.", "Messaging", false, false, true, false),
        new Field("send_direct_files", "Send direct files", "Enables file uploads to individual users. Enabled by default.", "Messaging", false, false, true, false),
        new Field("send_channel_pictures", "Send channel pictures", "Enables channel picture uploads. Channel picture power also applies. Enabled by default.", "Messaging", false, false, true, false),
        new Field("send_channel_files", "Send channel files", "Enables channel file uploads. Channel file power also applies. Enabled by default.", "Messaging", false, false, true, false),
        new Field("send_direct_messages", "Send direct messages", "Enables text messages to individual users. Direct message power must also reach the receiver's protection. Channel keys and read receipts need no permission. Enabled by default.", "Messaging", false, false, true, false),
        new Field("send_channel_messages", "Send channel messages", "Enables text messages in the user's channel. Channel message power also applies. Enabled by default.", "Messaging", false, false, true, false),
        new Field("send_pokes", "Send pokes", "Enables pokes. Poke power must also reach the receiver's protection. Enabled by default.", "Messaging", false, false, true, false),
        new Field("view_channel_members", "View channel members", "Shows who is in channels the user is not in. Channel view power must also reach the channel's requirement. Users always see the members of their own channel. Enabled by default.", "Channels", false, false, true, false),
        new Field("view_channel_descriptions", "View channel descriptions", "Shows the description of channels the user is not in. Channel view power must also reach the channel's requirement. Enabled by default.", "Channels", false, false, true, false),
        new Field("create_music_bots", "Create music bots", "Adds a music bot when the server enables bot audio.", "Music bots", false, false, false, false),
        new Field("delete_music_bots", "Delete music bots", "Removes bots and their uploaded songs.", "Music bots", false, false, false, false),
        new Field("upload_music_tracks", "Upload music tracks", "Adds MP3 files to a music bot.", "Music bots", false, false, false, false),
        new Field("delete_music_tracks", "Delete music tracks", "Removes songs from a music bot.", "Music bots", false, false, false, false),
        new Field("control_music_playback", "Control music playback", "Plays, pauses and skips songs without granting upload or delete rights.", "Music bots", false, false, false, false),
        new Field("use_reserved_slots", "Use reserved slots", "May use the slots reserved in server settings. The server total stays unchanged.", "Information and access", false, false, false, false),
        new Field("create_channel_groups", "Create channel groups", "Creates channel groups without granting rights you do not hold. Assignment has a separate permission.", "Channel group management", false, false, false, false),
        new Field("edit_channel_groups", "Edit channel groups", "Changes channel group names, icons and permissions within your management power.", "Channel group management", false, false, false, false),
        new Field("delete_channel_groups", "Delete channel groups", "Deletes custom channel groups within your management power and removes their assignments.", "Channel group management", false, false, false, false),
        new Field("talk_power", "Talk power", "", "Channels", true, true, false, false),
        new Field("stream_power", "Video streaming power", "", "Channels", true, true, false, false),
        new Field("channel_message_power", "Channel message power", "", "Channels", true, true, false, false),
        new Field("channel_picture_power", "Channel picture power", "", "Channels", true, true, false, false),
        new Field("channel_file_power", "Channel file power", "", "Channels", true, true, false, false),
        new Field("channel_view_power", "Channel view power", "", "Channels", true, false, false, false),
        new Field("channel_create_power", "Channel create power", "", "Channels", true, true, false, false),
        new Field("direct_message_power", "Direct message power", "", "Messaging", true, false, false, false),
        new Field("direct_message_protection", "Protection from direct messages", "", "Messaging", true, false, false, false),
        new Field("direct_picture_power", "Direct picture power", "", "Messaging", true, false, false, false),
        new Field("direct_picture_protection", "Protection from direct pictures", "", "Messaging", true, false, false, false),
        new Field("direct_file_power", "Direct file power", "", "Messaging", true, false, false, false),
        new Field("direct_file_protection", "Protection from direct files", "", "Messaging", true, false, false, false),
        new Field("poke_power", "Poke power", "", "Messaging", true, false, false, false),
        new Field("poke_protection", "Protection from pokes", "", "Messaging", true, false, false, false),
        new Field("kick_power", "Kick power", "", "Moderation", true, false, false, false),
        new Field("kick_protection", "Protection from kicks", "", "Moderation", true, false, false, false),
        new Field("ban_power", "Ban power", "", "Moderation", true, false, false, false),
        new Field("ban_protection", "Protection from bans", "", "Moderation", true, false, false, false),
        new Field("group_management_power", "Group management power", "", "Groups", true, true, false, false),
        new Field("permission_edit_power", "Permission editing power", "", "Groups", true, false, false, false),
        new Field("group_protection", "Protection from group changes", "", "Groups", true, true, false, false),
        new Field("move_power", "Move power", "", "Moderation", true, true, false, false),
        new Field("move_protection", "Protection from moves", "", "Moderation", true, true, false, false),
        new Field("moderation_power", "Moderation power", "", "Moderation", true, true, false, false),
        new Field("moderation_protection", "Protection from mutes and stream stops", "", "Moderation", true, true, false, false)
    ));

    static final List<Field> CHANNEL = Collections.unmodifiableList(Arrays.asList
    (
        new Field("bypass_channel_password", "Bypass this channel’s password", "Re-enter this assigned channel without its password, including after reconnecting. Other channel rights still require being inside the channel.", "Channel actions", false, false, false, true),
        new Field("edit_channel_name", "Edit channel name", "Change the name of the assigned channel.", "Channel actions", false, false, false, true),
        new Field("edit_channel_description", "Edit channel description", "Change the description of the assigned channel.", "Channel actions", false, false, false, true),
        new Field("edit_channel_password", "Edit channel password", "Change the password of the assigned channel.", "Channel actions", false, false, false, true),
        new Field("edit_channel_powers", "Edit required channel powers", "Change the talk, message, picture and streaming requirements of the assigned channel.", "Channel actions", false, false, false, true),
        new Field("edit_channel_options", "Edit channel options", "Change audio, streaming, capacity and icon settings for the assigned channel.", "Channel actions", false, false, false, true),
        new Field("delete_channel", "Delete this channel", "Delete the assigned channel. This does not grant permission to delete its child channels.", "Channel actions", false, false, false, true),
        new Field("assign_channel_groups", "Assign channel groups", "Assign or remove groups for other users in the same channel, within your assignment power.", "Channel actions", false, false, false, true),
        new Field("kick_from_channel", "Kick users from this channel", "Remove users from this channel by returning them to root. This does not grant moves to other channels.", "Channel actions", false, false, false, true),
        new Field("mute_clients", "Mute audio in this channel", "Temporarily mute another user's microphone and MP3 audio in this channel.", "Channel actions", false, false, false, true),
        new Field("stop_client_streams", "Stop streams in this channel", "Stop another user's screen or video stream in this channel.", "Channel actions", false, false, false, true),
        new Field("channel_group_assignment_power", "Channel group assignment power", "Must reach the other user's group protection. You cannot assign rights or powers you do not hold.", "Channel powers", true, false, false, true),
        new Field("channel_group_protection", "Channel group assignment protection", "Power required to change this user's channel group assignment.", "Channel powers", true, false, false, true),
        new Field("talk_power", "Talk power", "Compared with this channel's talk requirement for microphone and MP3 audio.", "Channel powers", true, false, false, true),
        new Field("channel_message_power", "Channel message power", "Compared with this channel's requirement for sending and editing text.", "Channel powers", true, false, false, true),
        new Field("channel_picture_power", "Channel picture power", "Compared with this channel's requirement for sending pictures.", "Channel powers", true, false, false, true),
        new Field("channel_file_power", "Channel file power", "Compared with this channel's requirement for sending files.", "Channel powers", true, false, false, true),
        new Field("channel_create_power", "Channel create power", "Compared with this channel's requirement for creating a channel inside it.", "Channel powers", true, false, false, true),
        new Field("stream_power", "Channel streaming power", "Compared with this channel's requirement for screen and video sharing.", "Channel powers", true, false, false, true),
        new Field("channel_kick_power", "Channel kick power", "Power to remove another user from this channel. It does not increase power to move users between channels.", "Channel powers", true, false, false, true),
        new Field("channel_kick_protection", "Channel kick protection", "Power required to remove this user from this channel. It does not protect against server kicks or moves.", "Channel powers", true, false, false, true),
        new Field("channel_moderation_power", "Channel mute and stream stop power", "Power to mute another user's audio or stop their stream in this channel.", "Channel powers", true, false, false, true),
        new Field("channel_moderation_protection", "Channel mute and stream stop protection", "Power required to mute this user's audio or stop their stream in this channel.", "Channel powers", true, false, false, true)
    ));

    static List<Field> forGroup(boolean channel)
    {
        return channel ? CHANNEL : SERVER;
    }

    private PermissionFields()
    {
    }
}
