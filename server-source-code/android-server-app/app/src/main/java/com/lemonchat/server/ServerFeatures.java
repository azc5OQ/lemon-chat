package com.lemonchat.server;

import org.json.JSONObject;

/**
 * The server features the host can switch on or off in the first setup and in Settings → Advanced. Each one is
 * a key of server_settings.json that the C server reads at start. The setup proposes the same values as the
 * desktop server's first setup (first_time_setup.c).
 */
final class ServerFeatures
{
    // settings keys, in the order the switches are shown, with the text of each switch
    static final String[] KEYS =
    {
        "is_voice_chat_active", "is_music_bot_audio_active", "is_video_streaming_active", "is_chat_picture_allowed",
        "is_file_upload_allowed", "is_avatar_upload_allowed", "is_link_preview_allowed", "is_typing_indicator_allowed",
        "is_display_country_flags_active"
    };
    static final String[] LABELS =
    {
        "Voice chat", "Music bots", "Video and screen sharing", "Pictures in chat", "File uploads", "Profile pictures (avatars)",
        "Link previews", "Show when someone is typing", "Country flags next to people"
    };
    static final String[] NOTES =
    {
        "People in the same channel can talk.", "Music bots can play songs into a channel, also when voice chat is off.",
        "Uses more battery, processor time and network capacity.", "Pictures sent in chat messages.",
        "Documents and other files sent in chat messages.", "People can upload a picture shown next to their name.",
        "The server fetches a short preview of links sent in chat.", "The unfinished message itself is not sent.",
        "Needs the country database included with the server."
    };

    // the values the desktop setup proposes; the C server uses the same values when a key is missing
    private static final boolean[] DEFAULTS = {true, true, false, true, false, false, false, false, false};

    final boolean[] values;

    private ServerFeatures(boolean[] values)
    {
        this.values = values;
    }

    static ServerFeatures Proposed()
    {
        return new ServerFeatures(DEFAULTS.clone());
    }

    /**
     * @brief Reads the features from saved settings; a missing key has the value the C server uses for it.
     *
     * @param settings saved server settings
     *
     * @return the features of these settings
     */
    static ServerFeatures FromSettings(JSONObject settings)
    {
        boolean[] values = new boolean[KEYS.length];
        for (int index = 0; index < KEYS.length; index++)
        {
            values[index] = settings.optBoolean(KEYS[index], DEFAULTS[index]);
        }
        return new ServerFeatures(values);
    }

    ServerFeatures Copy()
    {
        return new ServerFeatures(values.clone());
    }

    boolean IsEnabled(String key)
    {
        return values[Index(key)];
    }

    void SetEnabled(String key, boolean isEnabled)
    {
        values[Index(key)] = isEnabled;
    }

    private static int Index(String key)
    {
        for (int index = 0; index < KEYS.length; index++)
        {
            if (KEYS[index].equals(key))
            {
                return index;
            }
        }
        throw new IllegalArgumentException("Unknown feature " + key);
    }

    /**
     * @brief Writes every feature into the settings.
     *
     * @param settings server settings to change
     *
     * @return void
     *
     * @throws Exception when the settings object cannot be changed
     */
    void WriteTo(JSONObject settings) throws Exception
    {
        for (int index = 0; index < KEYS.length; index++)
        {
            settings.put(KEYS[index], values[index]);
        }
    }
}
