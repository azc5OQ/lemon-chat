package com.lemonchat.server;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import org.json.JSONObject;

/**
 * Keeps the Android choice of how other devices get the browser page (client.html) separate from the settings saved
 * by the C server: not at all, over HTTP in the local network, or over HTTPS through Tailscale Funnel.
 * Before each start, Prepare writes the matching values into server_settings.json and copies the page from the APK.
 */
final class HostingSettings
{
    static final String PAGE_NONE = "none";
    static final String PAGE_HTTP = "http";
    static final String PAGE_FUNNEL = "funnel";
    static final int DEFAULT_PAGE_PORT = 8080;

    private static SharedPreferences Preferences(Context context)
    {
        return context.getSharedPreferences("hosting", Context.MODE_PRIVATE);
    }

    /**
     * @brief Returns how the browser page is served. Before this choice existed, the app saved only whether Funnel
     *        was on; that value is read when no choice has been saved.
     *
     * @param context any context of the app
     *
     * @return PAGE_NONE, PAGE_HTTP or PAGE_FUNNEL
     */
    static String WebPage(Context context)
    {
        SharedPreferences preferences = Preferences(context);
        return preferences.getString("web_page", preferences.getBoolean("funnel", false) ? PAGE_FUNNEL : PAGE_NONE);
    }

    static int WebPagePort(Context context)
    {
        return Preferences(context).getInt("web_page_port", DEFAULT_PAGE_PORT);
    }

    /**
     * @brief Returns whether the served page contains the chat port and the join passwords, so that a visitor
     *        connects without typing them.
     *
     * @param context any context of the app
     *
     * @return true when the page contains them
     */
    static boolean IsPageConfigEmbedded(Context context)
    {
        return Preferences(context).getBoolean("is_page_config_embedded", true);
    }

    static boolean IsFunnelEnabled(Context context)
    {
        return WebPage(context).equals(PAGE_FUNNEL);
    }

    /**
     * @brief Saves how the browser page is served.
     *
     * @param context any context of the app
     * @param page PAGE_NONE, PAGE_HTTP or PAGE_FUNNEL
     * @param port HTTP port for PAGE_HTTP
     * @param isConfigEmbedded whether the page contains the chat port and the join passwords
     *
     * @return void
     */
    static void SetWebPage(Context context, String page, int port, boolean isConfigEmbedded)
    {
        Preferences(context).edit().putString("web_page", page).putInt("web_page_port", port)
            .putBoolean("is_page_config_embedded", isConfigEmbedded).putBoolean("funnel", page.equals(PAGE_FUNNEL)).apply();
    }

    /**
     * @brief Switches Funnel on or off and keeps the HTTP port and the page setting.
     *
     * @param context any context of the app
     * @param isEnabled true for Funnel, false to stop serving the page
     *
     * @return void
     */
    static void SetFunnelEnabled(Context context, boolean isEnabled)
    {
        SetWebPage(context, isEnabled ? PAGE_FUNNEL : PAGE_NONE, WebPagePort(context), IsPageConfigEmbedded(context));
    }

    /**
     * @brief Writes the hosting values into the saved server settings and copies the page when it is served.
     *        Funnel needs WebSocket audio, because Funnel does not forward UDP; the audio choice from before
     *        Funnel is kept and restored when Funnel is switched off.
     *
     * @param context context supplying assets and private storage
     *
     * @return settings saved for the native server
     *
     * @throws Exception if the helper is unavailable or a hosting file cannot be read or saved
     */
    static JSONObject Prepare(Context context) throws Exception
    {
        JSONObject settings = ServerSettings.Read(context.getFilesDir());
        SharedPreferences preferences = Preferences(context);
        String page = WebPage(context);
        boolean isFunnel = page.equals(PAGE_FUNNEL);

        // common values
        settings.put("is_tailscale_funnel_enabled", false);
        settings.put("is_external_wss_enabled", isFunnel);
        settings.put("is_client_http_enabled", !page.equals(PAGE_NONE));
        settings.put("is_stunnel_enabled", false);
        settings.put("is_https_enabled", false);

        // Funnel values
        if (isFunnel)
        {
            File helper = new File(context.getApplicationInfo().nativeLibraryDir, "liblemon-funnel.so");
            if (!helper.canExecute())
            {
                throw new IllegalStateException("This APK does not include the Tailscale helper.");
            }
            if (!preferences.contains("audio_before_funnel"))
            {
                preferences.edit().putString("audio_before_funnel", ServerSettings.AudioTransport(settings)).apply();
            }
            settings.put("wss_port", 8443);
            settings.put("http_port", settings.getInt("websocket_port") == 8080 ? 8081 : 8080);
            settings.put("is_client_config_embedded", false);
            WriteAudioTransport(settings, "websocket");
            settings.put("is_same_ip_address_allowed", true);
        }

        // audio from before Funnel
        else if (preferences.contains("audio_before_funnel"))
        {
            WriteAudioTransport(settings, preferences.getString("audio_before_funnel", "auto"));
            preferences.edit().remove("audio_before_funnel").apply();
        }

        // local network values
        if (page.equals(PAGE_HTTP))
        {
            int port = WebPagePort(context);
            if (port < 1024 || port > 65535 || port == settings.getInt("websocket_port"))
            {
                throw new IllegalArgumentException("Choose a web page port from 1024 to 65535 that differs from the chat port.");
            }
            settings.put("http_port", port);
            settings.put("is_client_config_embedded", IsPageConfigEmbedded(context));
        }

        // copy the page
        if (!page.equals(PAGE_NONE))
        {
            settings.put("http_webroot", context.getFilesDir().getAbsolutePath());
            CopyPage(context);
        }
        ServerSettings.Write(context.getFilesDir(), settings);
        return settings;
    }

    /**
     * @brief Writes the legacy audio mode and the two transport flags the C server reads.
     *
     * @param settings server settings to change
     * @param transport "webrtc", "auto", "websocket" or "disabled"
     *
     * @return void
     *
     * @throws Exception when the settings object cannot be changed
     */
    static void WriteAudioTransport(JSONObject settings, String transport) throws Exception
    {
        settings.put("audio_transport", transport);
        settings.put("is_audio_over_websocket_allowed", transport.equals("auto") || transport.equals("websocket"));
        settings.put("is_audio_over_webrtc_allowed", transport.equals("auto") || transport.equals("webrtc"));
    }

    /**
     * @brief Copies client.html from the APK into the app's files folder, which the C server serves.
     *
     * @param context context supplying assets and private storage
     *
     * @return void
     *
     * @throws Exception when the page cannot be read or written
     */
    private static void CopyPage(Context context) throws Exception
    {
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
}
