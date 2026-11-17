package com.lemonchat.model;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** validated general settings without administrator or connection passwords */
public final class AdminSettings
{
    public static final class Field
    {
        public final String key;
        public final String label;
        public final String section;
        public final String help;
        public final int kind;
        public final long minimum;
        public final long maximum;
        public final List<String> choices;

        private Field(String key, String label, String section, String help, int kind, long minimum, long maximum, String... choices)
        {
            this.key = key;
            this.label = label;
            this.section = section;
            this.help = help;
            this.kind = kind;
            this.minimum = minimum;
            this.maximum = maximum;
            this.choices = Collections.unmodifiableList(Arrays.asList(choices));
        }
    }

    public static final int TOGGLE = 0;
    public static final int NUMBER = 1;
    public static final int CHOICE = 2;
    public static final int TEXT = 3;
    public static final List<Field> FIELDS;
    public final Map<String, Object> values;
    public final long maxSlots;
    public final long usedSlots;
    public final long ramBytes;
    public final long uptimeSeconds;
    public final int connectionPasswordCount;

    static
    {
        ArrayList<Field> fields = new ArrayList<>();
        fields.add(number("max_channel_count", "Channel limit", "General", "Delete channels outside the new limit before lowering it", 1, Integer.MAX_VALUE));
        fields.add(number("reserved_slots", "Reserved connection slots", "General", "Must be fewer than the server's total slots", 0, Integer.MAX_VALUE));
        fields.add(toggle("is_temp_channel_creation_allowed", "Allow temporary channels", "General", "Applies when server groups are disabled"));
        fields.add(toggle("is_display_country_flags_active", "Show country flags", "General", "Show the country people connect from"));
        fields.add(toggle("is_admin_country_flag_hidden", "Hide administrator country flags", "General", "Applies to confirmed administrators"));
        fields.add(toggle("is_hide_clients_in_password_protected_channels_active", "Hide protected channel members", "General", "People outside a protected channel cannot see its members"));
        fields.add(toggle("is_typing_indicator_allowed", "Allow typing indicators", "Messages", "People can see when someone is writing"));
        fields.add(toggle("is_client_rename_allowed", "Allow name changes", "Messages", "People can change their name while connected"));
        fields.add(toggle("is_link_preview_allowed", "Allow link previews", "Messages", "Clients may request previews for links"));
        fields.add(toggle("is_sending_text_to_idle_clients_allowed", "Allow messages to away users", "Messages", "Away users remain available for text"));
        fields.add(toggle("is_private_messaging_allowed", "Allow private messages", "Messages", "People can start direct conversations"));
        fields.add(toggle("is_voice_chat_active", "Enable voice chat", "Voice and video", "Allow channel voice"));
        fields.add(new Field("audio_connections", "Audio and video connection", "Voice and video", "Use WebSocket for HTTP tunnels", CHOICE, 0, 0, "both", "websocket", "webrtc"));
        fields.add(toggle("is_music_bot_audio_active", "Enable music bot audio", "Voice and video", "Allow bots to play music"));
        fields.add(toggle("is_video_streaming_active", "Enable video streams", "Voice and video", "Allow supported clients to stream video"));
        fields.add(toggle("is_music_bot_marquee_visible_to_everyone", "Show music titles in other channels", "Voice and video", "Otherwise titles are visible only in the bot's channel"));
        fields.add(number("webrtc_datachannel_cooldown_seconds", "WebRTC reconnect delay (seconds)", "Voice and video", "Minimum delay before creating another data channel", 0, 86400));
        fields.add(toggle("is_file_upload_allowed", "Allow file transfers", "Files and pictures", "Allow chat attachments"));
        fields.add(number("file_upload_max_size_mb", "Maximum file size (MiB)", "Files and pictures", "One MiB is 1,048,576 bytes", 1, 100));
        fields.add(toggle("is_chat_picture_allowed", "Allow chat pictures", "Files and pictures", "Allow pictures in conversations"));
        fields.add(number("chat_picture_max_size_mb", "Maximum picture size (MiB)", "Files and pictures", "Limit for each chat picture", 1, 15));
        fields.add(number("avatar_max_size_kb", "Maximum avatar size (KiB)", "Files and pictures", "Limit for each uploaded avatar", 1, 90));
        fields.add(number("icon_max_size_bytes", "Maximum icon size (bytes)", "Files and pictures", "Limit for each server icon", 1000, 30000));
        fields.add(toggle("is_blocking_direct_chat_pictures_active", "Let users block private pictures", "Files and pictures", "Recipients choose whether to receive them"));
        fields.add(toggle("is_blocking_direct_chat_files_active", "Let users block private files", "Files and pictures", "Recipients choose whether to receive them"));
        fields.add(toggle("is_blocking_channel_chat_pictures_active", "Let users block channel pictures", "Files and pictures", "Recipients choose whether to receive them"));
        fields.add(toggle("is_blocking_channel_chat_files_active", "Let users block channel files", "Files and pictures", "Recipients choose whether to receive them"));
        fields.add(toggle("is_same_ip_address_allowed", "Allow shared IP addresses", "Connections", "Allow several people on the same network"));
        fields.add(toggle("is_fast_reconnect_allowed", "Allow fast reconnect", "Connections", "Allow the existing reconnect protocol"));
        fields.add(toggle("is_identity_takeover_allowed", "Allow identity takeover", "Connections", "A new connection can replace another using the same identity"));
        fields.add(toggle("is_websocket_ping_active", "Use WebSocket ping", "Connections", "Check the underlying WebSocket connection"));
        fields.add(toggle("is_connection_check_enforced", "Enforce connection checks", "Connections", "Disconnect clients that stop answering"));
        fields.add(number("max_concurrent_logins", "Concurrent logins", "Connections", "Maximum number of logins being processed at once", 1, 64));
        fields.add(number("minimum_rsa_key_bits", "Minimum identity key size", "Connections", "Applies to new connections", 2048, 8192));
        fields.add(toggle("is_minimum_rsa_key_bits_announced", "Announce minimum identity size", "Connections", "Tell rejected clients which key size is required"));
        fields.add(number("dh_exchange_count", "Key exchanges per login", "Encryption", "Applies to new connections", 1, 4));
        fields.add(number("session_encryption_layer_count", "Session encryption layers", "Encryption", "Excludes the optional random layer", 1, 8));
        fields.add(new Field("session_cipher", "Session cipher", "Encryption", "Existing connections retain their negotiated keys", CHOICE, 0, 0, "aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"));
        fields.add(toggle("is_random_cipher_layer_enabled", "Add a random cipher layer", "Encryption", "Adds another layer to each new connection"));
        fields.add(toggle("is_channel_key_reuse_enabled", "Allow channel key reuse", "Encryption", "Controls reuse for large channels"));
        fields.add(toggle("is_channel_key_reuse_on_join_allowed", "Allow reuse when someone joins", "Encryption", "Uses the joining threshold below"));
        fields.add(number("channel_key_reuse_join_min_clients", "Joining threshold", "Encryption", "Minimum channel population for reuse", 30, 1000000));
        fields.add(toggle("is_channel_key_reuse_on_leave_allowed", "Allow reuse when someone leaves", "Encryption", "Uses the leaving threshold below"));
        fields.add(number("channel_key_reuse_leave_min_clients", "Leaving threshold", "Encryption", "Minimum channel population for reuse", 30, 1000000));
        fields.add(toggle("is_client_http_enabled", "Serve the browser client", "Browser page", "Controls the server's existing HTTP page"));
        fields.add(toggle("is_client_http_blank_when_disabled", "Return a blank disabled page", "Browser page", "Otherwise the disabled HTTP page responds with Not Found"));
        fields.add(toggle("is_client_config_embedded", "Include connection details", "Browser page", "The downloaded page includes the server's connection passwords"));
        fields.add(toggle("is_start_page_connect_button_enabled", "Show a Connect start page", "Browser page", "The page waits for a click before connecting"));
        fields.add(toggle("is_start_page_import_identity_button_enabled", "Offer identity import on the start page", "Browser page", "Requires the Connect start page"));
        fields.add(new Field("default_theme", "Desktop default theme", "Browser page", "Theme identifier, or empty for the page default", TEXT, 0, 31));
        fields.add(new Field("default_mobile_theme", "Phone default theme", "Browser page", "Theme identifier, or empty for the page default", TEXT, 0, 31));
        FIELDS = Collections.unmodifiableList(fields);
    }

    public AdminSettings(JSONObject source) throws JSONException
    {
        LinkedHashMap<String, Object> result = new LinkedHashMap<>();
        for (Field field : FIELDS)
        {
            if (source.has(field.key))
            {
                Object value = source.get(field.key);
                validate(field, value);
                result.put(field.key, value);
            }
        }
        values = Collections.unmodifiableMap(result);
        maxSlots = readCount(source, "max_slots");
        usedSlots = readCount(source, "used_slots");
        ramBytes = readCount(source, "ram_bytes");
        uptimeSeconds = readCount(source, "uptime_seconds");
        connectionPasswordCount = (int)Math.min(100, readCount(source, "connection_password_count"));
    }

    /**
     * builds only changed settings and validates server capacity
     *
     * @param edited visible draft values
     * @param includeMusic whether to save music bot playlists too
     * @return independent save request body
     */
    public JSONObject changes(Map<String, Object> edited, boolean includeMusic) throws JSONException
    {
        JSONObject result = new JSONObject();
        result.put("type", "save_server_settings");
        result.put("include_music_bots", includeMusic);
        for (String key : edited.keySet())
        {
            Field field = field(key);
            if (field == null || !values.containsKey(key))
            {
                throw new IllegalArgumentException("This setting is not available on the current server");
            }
            Object value = edited.get(key);
            validate(field, value);
            if (key.equals("reserved_slots") && ((Number)value).longValue() >= maxSlots)
            {
                throw new IllegalArgumentException("Reserved slots must be fewer than " + maxSlots);
            }
            if (!String.valueOf(value).equals(String.valueOf(values.get(key))))
            {
                result.put(key, value);
            }
        }
        return result;
    }

    public static Field field(String key)
    {
        for (Field field : FIELDS)
        {
            if (field.key.equals(key))
            {
                return field;
            }
        }
        return null;
    }

    private static Field toggle(String key, String label, String section, String help)
    {
        return new Field(key, label, section, help, TOGGLE, 0, 0);
    }

    private static Field number(String key, String label, String section, String help, long minimum, long maximum)
    {
        return new Field(key, label, section, help, NUMBER, minimum, maximum);
    }

    private static void validate(Field field, Object value)
    {
        boolean valid;
        if (field.kind == TOGGLE)
        {
            valid = value instanceof Boolean;
        }
        else if (field.kind == NUMBER)
        {
            valid = value instanceof Number && ((Number)value).doubleValue() == ((Number)value).longValue() && ((Number)value).longValue() >= field.minimum && ((Number)value).longValue() <= field.maximum;
        }
        else if (field.kind == CHOICE)
        {
            valid = value instanceof String && field.choices.contains(value);
        }
        else
        {
            valid = value instanceof String && ((String)value).matches("[a-z0-9-]{0,31}");
        }
        if (!valid)
        {
            throw new IllegalArgumentException(field.label + (field.kind == NUMBER ? " must be a whole number from " + field.minimum + " to " + field.maximum : " has an unsupported value"));
        }
    }

    private static long readCount(JSONObject source, String key) throws JSONException
    {
        Object value = source.opt(key);
        if (value == null || value == JSONObject.NULL)
        {
            return 0;
        }
        if (!(value instanceof Number) || !Double.isFinite(((Number)value).doubleValue()) || ((Number)value).doubleValue() < 0 || ((Number)value).doubleValue() > 9007199254740991L)
        {
            throw new JSONException("Invalid server usage value");
        }
        return ((Number)value).longValue();
    }
}
