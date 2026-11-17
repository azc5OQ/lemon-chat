package com.lemonchat.settings;

import android.content.Context;
import android.content.SharedPreferences;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** validated local preferences shared by the screens and service */
public final class LocalSettings
{
    private static final String STORE = "local-settings";
    private static final Map<String, Rule> RULES = new LinkedHashMap<>();
    private final Map<String, Object> values;

    static
    {
        number("native_ui_scale", 100, 70, 130);
        number("lemon_chat_display_scale", 100, 50, 200);
        toggle("lemon_show_message_avatars", true);
        toggle("lemon_use_theme_chat_formatting", false);
        toggle("lemon_show_flags", true);
        toggle("lemon_hide_server_group_icons", false);
        toggle("lemon_hide_channel_group_icons", false);
        toggle("lemon_hide_channel_icons", false);
        toggle("lemon_show_channel_people", false);
        toggle("lemon_channels_flat", false);
        toggle("lemon_custom_cursor", true);
        toggle("lemon_strip_vertical", false);
        toggle("lemon_strip_neighbors", false);
        toggle("lemon_audio_availability", true);
        toggle("lemon_chat_reactions", false);
        toggle("lemon_chat_replies", true);
        toggle("lemon_auto_scroll", true);
        toggle("lemon_is_public_key_info_hidden", true);
        toggle("lemon_limit_chat_messages", false);
        number("lemon_chat_message_limit", 1000, 1, 100000);
        toggle("lemon_chat_search", true);
        toggle("lemon_seen_indicator", true);
        toggle("lemon_send_seen", true);
        toggle("lemon_continuous_mic", false);
        number("native_ptt_key_code", 45, 1, 1000);
        toggle("lemon_ptt_disable_while_typing", true);
        RULES.put("lemon_mic_device_id", new Rule("", 0, 256, null));
        toggle("lemon_hide_mic", false);
        toggle("lemon_hide_mobile_mic", false);
        toggle("lemon_show_mp3_button", true);
        toggle("lemon_sound_effects", true);
        choice("lemon_audio_connection", "webrtc", "webrtc", "websocket");
        number("lemon_chat_image_max_width", 600, 32, 4096);
        number("lemon_chat_image_max_height", 480, 32, 4096);
        toggle("lemon_block_direct_chat_pictures", false);
        toggle("lemon_block_direct_chat_files", false);
        toggle("lemon_block_channel_chat_pictures", false);
        toggle("lemon_block_channel_chat_files", false);
        choice("lemon_rsa_key_bits", "2048", "2048", "3072", "4096", "6144", "8192");
        toggle("lemon_show_log_button", false);
        toggle("native_file_logging", false);
    }

    private LocalSettings(Map<String, Object> values)
    {
        this.values = Collections.unmodifiableMap(new LinkedHashMap<>(values));
    }

    /**
     * reads saved preferences and ignores invalid stored values
     *
     * @param context application or screen context
     * @return an immutable settings snapshot
     */
    public static LocalSettings load(Context context)
    {
        Map<String, ?> stored = context.getSharedPreferences(STORE, Context.MODE_PRIVATE).getAll();
        Map<String, Object> result = defaults();
        for (Map.Entry<String, Rule> entry : RULES.entrySet())
        {
            Object value = stored.get(entry.getKey());
            if (value != null)
            {
                try
                {
                    result.put(entry.getKey(), entry.getValue().validate(value));
                }
                catch (IllegalArgumentException ignored)
                {
                    // keep the default when stored data is invalid
                }
            }
        }
        if (!stored.containsKey("lemon_hide_mic"))
        {
            SharedPreferences previous = context.getSharedPreferences("preferences", Context.MODE_PRIVATE);
            result.put("lemon_hide_mic", !previous.getBoolean("show-microphone", true));
        }
        return new LocalSettings(result);
    }

    /**
     * validates a draft without changing saved preferences
     *
     * @param draft proposed values
     * @return validated settings
     * @throws IllegalArgumentException when a known value is invalid
     */
    public static LocalSettings validate(Map<String, ?> draft)
    {
        if (draft == null)
        {
            throw new IllegalArgumentException("Settings are missing");
        }
        Map<String, Object> result = defaults();
        for (Map.Entry<String, Rule> entry : RULES.entrySet())
        {
            if (draft.containsKey(entry.getKey()))
            {
                try
                {
                    result.put(entry.getKey(), entry.getValue().validate(draft.get(entry.getKey())));
                }
                catch (IllegalArgumentException error)
                {
                    throw new IllegalArgumentException("Invalid setting: " + entry.getKey(), error);
                }
            }
        }
        return new LocalSettings(result);
    }

    /**
     * checks one field without changing the rest of the draft
     *
     * @param key preference name
     * @param value proposed value
     * @return validated value
     * @throws IllegalArgumentException when the field is unknown or invalid
     */
    public static Object validateValue(String key, Object value)
    {
        Rule rule = RULES.get(key);
        if (rule == null)
        {
            throw new IllegalArgumentException("Unknown setting: " + key);
        }
        return rule.validate(value);
    }

    /**
     * commits a complete settings snapshot on the storage worker
     *
     * @param context application context
     * @return whether Android saved the preferences
     */
    public boolean save(Context context)
    {
        SharedPreferences.Editor editor = context.getSharedPreferences(STORE, Context.MODE_PRIVATE).edit();
        for (Map.Entry<String, Object> entry : values.entrySet())
        {
            Object value = entry.getValue();
            if (value instanceof Boolean)
            {
                editor.putBoolean(entry.getKey(), (Boolean) value);
            }
            else if (value instanceof Integer)
            {
                editor.putInt(entry.getKey(), (Integer) value);
            }
            else
            {
                editor.putString(entry.getKey(), (String) value);
            }
        }
        return editor.commit();
    }

    public boolean getBoolean(String key)
    {
        Object value = values.get(key);
        if (!(value instanceof Boolean))
        {
            throw new IllegalArgumentException("Unknown switch: " + key);
        }
        return (Boolean) value;
    }

    public int getInt(String key)
    {
        Object value = values.get(key);
        if (value instanceof Integer)
        {
            return (Integer) value;
        }
        if ("lemon_rsa_key_bits".equals(key))
        {
            return Integer.parseInt((String) value);
        }
        throw new IllegalArgumentException("Unknown number: " + key);
    }

    public String getString(String key)
    {
        Object value = values.get(key);
        if (!(value instanceof String))
        {
            throw new IllegalArgumentException("Unknown choice: " + key);
        }
        return (String) value;
    }

    /** @return an independent draft containing every current value */
    public Map<String, Object> asMap()
    {
        return new LinkedHashMap<>(values);
    }

    private static Map<String, Object> defaults()
    {
        Map<String, Object> result = new LinkedHashMap<>();
        for (Map.Entry<String, Rule> entry : RULES.entrySet())
        {
            result.put(entry.getKey(), entry.getValue().fallback);
        }
        return result;
    }

    private static void toggle(String key, boolean fallback)
    {
        RULES.put(key, new Rule(fallback, 0, 0, null));
    }

    private static void number(String key, int fallback, int minimum, int maximum)
    {
        RULES.put(key, new Rule(fallback, minimum, maximum, null));
    }

    private static void choice(String key, String fallback, String... options)
    {
        RULES.put(key, new Rule(fallback, 0, 256, Arrays.asList(options)));
    }

    private static final class Rule
    {
        final Object fallback;
        final int minimum;
        final int maximum;
        final List<String> choices;

        Rule(Object fallback, int minimum, int maximum, List<String> choices)
        {
            this.fallback = fallback;
            this.minimum = minimum;
            this.maximum = maximum;
            this.choices = choices;
        }

        Object validate(Object value)
        {
            if (fallback instanceof Boolean && value instanceof Boolean)
            {
                return value;
            }
            if (fallback instanceof Integer && value instanceof Number)
            {
                Number number = (Number) value;
                long integer = number.longValue();
                if (number.doubleValue() == integer && integer >= minimum && integer <= maximum)
                {
                    return (int) integer;
                }
            }
            if (fallback instanceof String && value instanceof String)
            {
                String text = (String) value;
                if (text.length() <= maximum && (choices == null || choices.contains(text)))
                {
                    return text;
                }
            }
            throw new IllegalArgumentException("Invalid preference value");
        }
    }
}
