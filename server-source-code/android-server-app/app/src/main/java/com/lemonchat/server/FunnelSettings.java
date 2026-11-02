package com.lemonchat.server;

import android.content.Context;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/** Keeps the Android hosting choice separate from settings saved by the C server. */
final class FunnelSettings
{
    static boolean IsEnabled(Context context)
    {
        return context.getSharedPreferences("hosting", Context.MODE_PRIVATE).getBoolean("funnel", false);
    }

    static void SetEnabled(Context context, boolean enabled)
    {
        context.getSharedPreferences("hosting", Context.MODE_PRIVATE).edit().putBoolean("funnel", enabled).apply();
    }

    /** Prepares the page and transport settings before the native server starts. */
    static JSONObject Prepare(Context context) throws Exception
    {
        JSONObject settings = ServerSettings.Read(context.getFilesDir());
        boolean enabled = IsEnabled(context);
        settings.put("is_tailscale_funnel_enabled", false);
        settings.put("is_external_wss_enabled", enabled);
        settings.put("is_client_http_enabled", enabled);
        if (enabled)
        {
            File helper = new File(context.getApplicationInfo().nativeLibraryDir, "liblemon-funnel.so");
            if (!helper.canExecute())
            {
                throw new IllegalStateException("This APK does not include the Tailscale helper.");
            }
            settings.put("wss_port", 8443);
            settings.put("http_port", settings.getInt("websocket_port") == 8080 ? 8081 : 8080);
            settings.put("http_webroot", context.getFilesDir().getAbsolutePath());
            settings.put("is_stunnel_enabled", false);
            settings.put("is_https_enabled", false);
            settings.put("is_client_config_embedded", false);
            settings.put("audio_transport", "websocket");
            settings.put("is_audio_over_websocket_allowed", true);
            settings.put("is_audio_over_webrtc_allowed", false);
            settings.put("is_same_ip_address_allowed", true);
            try (InputStream input = context.getAssets().open("client.html"))
            {
                AtomicFile page = new AtomicFile(new File(context.getFilesDir(), "client.html"));
                FileOutputStream output = page.startWrite();
                try
                {
                    byte[] buffer = new byte[32768];
                    int length;
                    while ((length = input.read(buffer)) != -1)
                    {
                        output.write(buffer, 0, length);
                    }
                    page.finishWrite(output);
                }
                catch (Exception error)
                {
                    page.failWrite(output);
                    throw error;
                }
            }
        }
        AtomicFile file = new AtomicFile(new File(context.getFilesDir(), "server_settings.json"));
        FileOutputStream output = file.startWrite();
        try
        {
            output.write(settings.toString(2).getBytes(StandardCharsets.UTF_8));
            file.finishWrite(output);
        }
        catch (Exception error)
        {
            file.failWrite(output);
            throw error;
        }
        return settings;
    }
}
