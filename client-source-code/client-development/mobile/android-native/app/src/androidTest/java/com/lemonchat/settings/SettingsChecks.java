package com.lemonchat.settings;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.SharedPreferences;
import java.util.Map;
import java.util.UUID;

/** checks saved preferences without touching the user's settings */
public final class SettingsChecks
{
    public static int verify(Context application)
    {
        String prefix = "settings-test-" + UUID.randomUUID() + "-";
        Context isolated = new ContextWrapper(application)
        {
            @Override
            public SharedPreferences getSharedPreferences(String name, int mode)
            {
                return super.getSharedPreferences(prefix + name, mode);
            }
        };
        try
        {
            LocalSettings initial = LocalSettings.load(isolated);
            check(initial.getInt("lemon_chat_display_scale") == 100 && initial.getBoolean("lemon_chat_replies") && !initial.getBoolean("lemon_chat_reactions"), "browser display defaults");
            check(initial.getString("lemon_audio_connection").equals("webrtc") && !initial.getBoolean("lemon_continuous_mic"), "transport and microphone defaults");
            Map<String, Object> draft = initial.asMap();
            draft.put("lemon_chat_display_scale", 150);
            draft.put("lemon_chat_reactions", true);
            draft.put("lemon_audio_connection", "websocket");
            draft.put("lemon_rsa_key_bits", "4096");
            draft.put("lemon_mic_device_id", "15:device");
            check(initial.getInt("lemon_chat_display_scale") == 100 && LocalSettings.load(isolated).getInt("lemon_chat_display_scale") == 100, "editing a draft does not apply it");
            LocalSettings selected = LocalSettings.validate(draft);
            check(selected.save(isolated), "settings commit");
            LocalSettings loaded = LocalSettings.load(isolated);
            check(loaded.asMap().equals(selected.asMap()), "every preference survives reload");
            check(loaded.getInt("lemon_rsa_key_bits") == 4096 && loaded.getString("lemon_mic_device_id").equals("15:device"), "typed identity and microphone values");
            draft.put("lemon_chat_display_scale", 0);
            reject(() -> LocalSettings.validate(draft), "invalid draft rejected");
            check(LocalSettings.load(isolated).getInt("lemon_chat_display_scale") == 150, "invalid draft preserves saved settings");
            check(LocalSettings.validateValue("lemon_chat_message_limit", 42).equals(42), "independent field validation");
            reject(() -> LocalSettings.validateValue("lemon_chat_message_limit", Double.NaN), "nonfinite number rejected");
            reject(() -> LocalSettings.validateValue("lemon_chat_image_max_width", 31), "image lower bound");
            reject(() -> LocalSettings.validateValue("lemon_chat_image_max_height", 4097), "image upper bound");
            reject(() -> LocalSettings.validateValue("lemon_rsa_key_bits", "1024"), "unsupported RSA size rejected");
            reject(() -> LocalSettings.validateValue("lemon_audio_connection", "external"), "unsupported transport rejected");
            reject(() -> LocalSettings.validateValue("lemon_mic_device_id", "x".repeat(257)), "device identifier bounded");
            isolated.getSharedPreferences("local-settings", Context.MODE_PRIVATE).edit().putString("lemon_chat_display_scale", "bad").putBoolean("lemon_hide_mic", true).commit();
            LocalSettings repaired = LocalSettings.load(isolated);
            check(repaired.getInt("lemon_chat_display_scale") == 100 && repaired.getBoolean("lemon_hide_mic"), "one damaged field does not discard valid settings");
            isolated.getSharedPreferences("local-settings", Context.MODE_PRIVATE).edit().remove("lemon_hide_mic").commit();
            isolated.getSharedPreferences("preferences", Context.MODE_PRIVATE).edit().putBoolean("show-microphone", false).commit();
            check(LocalSettings.load(isolated).getBoolean("lemon_hide_mic"), "previous microphone visibility migrates");
            return 17;
        }
        finally
        {
            application.deleteSharedPreferences(prefix + "local-settings");
            application.deleteSharedPreferences(prefix + "preferences");
        }
    }

    private static void reject(Runnable operation, String label)
    {
        boolean rejected = false;
        try
        {
            operation.run();
        }
        catch (IllegalArgumentException expected)
        {
            rejected = true;
        }
        check(rejected, label);
    }

    private static void check(boolean condition, String label)
    {
        if (!condition)
        {
            throw new AssertionError(label);
        }
    }
}
