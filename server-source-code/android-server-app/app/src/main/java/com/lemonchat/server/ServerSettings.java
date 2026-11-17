package com.lemonchat.server;

import android.util.AtomicFile;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Base64;

/** Uses the existing C server format. No passwords are bundled in the APK. */
final class ServerSettings
{
    // the same limit as the C server's settings reader (server_settings.h)
    private static final int MAX_SETTINGS_BYTES = 64 * 1024 * 1024;

    /**
     * detects saved settings and interrupted atomic writes
     *
     * @param directory private server directory
     * @return whether setup must preserve an existing configuration
     */
    static boolean Exists(File directory)
    {
        return new File(directory, "server_settings.json").exists() || new File(directory, "server_settings.json.bak").exists() || new File(directory, "server_settings.json.new").exists();
    }

    /**
     * creates settings without replacing an imported or recovered configuration
     *
     * @param directory private server directory
     * @param draft completed first setup choices
     * @throws Exception when settings already exist or validation fails
     */
    static synchronized void Create(File directory, SetupDraft draft) throws Exception
    {
        if (Exists(directory))
        {
            throw new IOException("Settings already exist on this phone. They have been kept. Return to the server screen to use them.");
        }
        Write(directory, draft.Settings());
    }

    static synchronized JSONObject Read(File directory) throws Exception
    {
        AtomicFile file = new AtomicFile(new File(directory, "server_settings.json"));
        JSONObject json;
        try (InputStream input = file.openRead())
        {
            json = new JSONObject(new String(ReadBytes(input, MAX_SETTINGS_BYTES), StandardCharsets.UTF_8));
        }
        int port = json.getInt("websocket_port");
        int slots = json.getInt("server_slots");
        AudioTransport(json);
        JoinPassword(json);
        if (json.getDouble("websocket_port") != port || json.getDouble("server_slots") != slots || port < 1024 ||
            port > 65535 || slots < 1 || slots > 100000 ||
            !json.getString("admin_password").matches("[A-Za-z0-9+/]{43}="))
        {
            throw new IllegalArgumentException("Invalid server settings.");
        }
        return json;
    }

    /**
     * bounds the file before decoding JSON
     *
     * @param input settings file stream
     * @param limit maximum byte count
     * @return complete file bytes
     * @throws IOException when the file cannot be read or exceeds the limit
     */
    static byte[] ReadBytes(InputStream input, int limit) throws IOException
    {
        ByteArrayOutputStream output = new ByteArrayOutputStream(Math.min(limit, 8192));
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) != -1)
        {
            if (count > limit - output.size())
            {
                throw new IOException("Server settings exceed the Android app's 64 MiB limit. The original file has not been changed.");
            }
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    static synchronized void Save(File directory, int port, int slots, String admin, String connect, ServerFeatures features, String transport) throws Exception
    {
        JSONObject json = Update(Exists(directory) ? Read(directory) : new JSONObject(), port, slots, admin, connect, features.IsEnabled("is_video_streaming_active"), transport);
        features.WriteTo(json);
        Write(directory, json);
    }

    /**
     * replaces settings only after their encoded size is accepted
     *
     * @param directory private server directory
     * @param json complete server settings
     * @throws Exception when the settings cannot be encoded or saved
     */
    static synchronized void Write(File directory, JSONObject json) throws Exception
    {
        byte[] content = json.toString(2).getBytes(StandardCharsets.UTF_8);
        if (content.length > MAX_SETTINGS_BYTES)
        {
            throw new IOException("Server settings exceed the Android app's 64 MiB limit. The original file has not been changed.");
        }
        AtomicFile file = new AtomicFile(new File(directory, "server_settings.json"));
        FileOutputStream output = null;
        try
        {
            output = file.startWrite();
            output.write(content);
            file.finishWrite(output);
        }
        catch (Exception error)
        {
            if (output != null)
            {
                file.failWrite(output);
            }
            throw error;
        }
    }

    /**
     * updates form fields while retaining the rest of the saved server state
     *
     * @param existing current settings object
     * @param port chat listener port
     * @param slots reserved client slots
     * @param admin new admin password or empty to retain it
     * @param connect first connection password
     * @param video whether video is allowed
     * @param transport selected audio transport mode
     * @return updated copy of the settings
     * @throws Exception when a form value is invalid
     */
    static JSONObject Update(JSONObject existing, int port, int slots, String admin, String connect, boolean video, String transport) throws Exception
    {
        if (port < 1024 || port > 65535)
        {
            throw new IllegalArgumentException("Choose a port from 1024 to 65535.");
        }
        if (slots < 1 || slots > 100000)
        {
            throw new IllegalArgumentException("Slots must be between 1 and 100000.");
        }
        if (!(transport.equals("webrtc") || transport.equals("auto") || transport.equals("websocket") || transport.equals("disabled")))
        {
            throw new IllegalArgumentException("Choose an audio connection mode.");
        }
        if (admin.getBytes(StandardCharsets.UTF_8).length > 50 || connect.getBytes(StandardCharsets.UTF_8).length > 255)
        {
            throw new IllegalArgumentException("Admin password: at most 50 UTF-8 bytes. Join password: at most 255.");
        }
        if (admin.indexOf('\0') >= 0 || connect.indexOf('\0') >= 0)
        {
            throw new IllegalArgumentException("Passwords cannot contain a null character.");
        }
        String oldConnect = JoinPassword(existing);
        if (!connect.equals(oldConnect) && (!existing.optString("music_bot_snapshot", "").isEmpty() || !existing.optString("music_bot_snapshot_previous", "").isEmpty()))
        {
            throw new IllegalArgumentException("Saved music uses the current join password. Start the server and change the password in a connected client's server settings so the music can be saved with the new password.");
        }
        if (!connect.equals(oldConnect) && !connect.equals(connect.trim()))
        {
            throw new IllegalArgumentException("Remove spaces at the beginning or end of the join password.");
        }
        JSONObject json = new JSONObject(existing.toString());
        boolean firstRun = !json.has("admin_password");
        if (!admin.isEmpty())
        {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(admin.getBytes(StandardCharsets.UTF_8));
            json.put("admin_password", Base64.getEncoder().encodeToString(digest));
            json.put("is_admin_password_initial", true);
        }
        else if (firstRun)
        {
            throw new IllegalArgumentException("Set an admin password for the first start.");
        }
        json.put("websocket_port", port);
        json.put("server_slots", slots);
        json.put("audio_transport", transport);
        json.put("is_audio_over_websocket_allowed", transport.equals("auto") || transport.equals("websocket"));
        json.put("is_audio_over_webrtc_allowed", transport.equals("auto") || transport.equals("webrtc"));
        JSONArray oldKeys = json.optJSONArray("keys");
        if (!connect.equals(oldConnect) || oldKeys == null)
        {
            if (connect.isEmpty())
            {
                json.put("keys", new JSONArray());
            }
            else if (oldKeys != null && oldKeys.length() > 0)
            {
                Object firstKey = oldKeys.get(0);
                if (firstKey instanceof JSONObject)
                {
                    ((JSONObject)firstKey).put("password", connect);
                }
                else
                {
                    oldKeys.put(0, connect);
                }
            }
            else
            {
                json.put("keys", new JSONArray().put(connect));
            }
        }
        if (firstRun)
        {
            json.put("is_voice_chat_active", true);
            json.put("is_video_streaming_active", false);
            json.put("is_music_bot_audio_active", false);
            json.put("is_same_ip_address_allowed", true);
            json.put("is_display_country_flags_active", false);
            json.put("is_country_blocking_active", false);
            json.put("is_wireguard_mode_active", false);
            json.put("is_stunnel_enabled", false);
            json.put("is_client_http_enabled", false);
            json.put("is_https_enabled", false);
            json.put("is_restart_on_crash_enabled", false);
        }
        json.put("is_video_streaming_active", video);
        return json;
    }

    /**
     * reads the first password without its cipher metadata
     *
     * @param settings saved server settings
     * @return first connection password or empty when no keys exist
     */
    static String JoinPassword(JSONObject settings)
    {
        JSONArray keys = settings.optJSONArray("keys");
        if (keys == null || keys.length() == 0)
        {
            return "";
        }
        Object first = keys.opt(0);
        if (first instanceof String)
        {
            return (String)first;
        }
        if (first instanceof JSONObject && ((JSONObject)first).opt("password") instanceof String)
        {
            return ((JSONObject)first).optString("password");
        }
        throw new IllegalArgumentException("The first server key has no valid password.");
    }

    /**
     * applies explicit transport flags after the legacy mode
     *
     * @param settings saved server settings
     * @return effective audio transport mode
     */
    static String AudioTransport(JSONObject settings)
    {
        String legacy = settings.optString("audio_transport", "auto");
        if (!(legacy.equals("webrtc") || legacy.equals("websocket") || legacy.equals("auto") || legacy.equals("disabled")))
        {
            throw new IllegalArgumentException("Invalid audio connection mode.");
        }
        boolean websocket = !legacy.equals("webrtc");
        boolean webrtc = !legacy.equals("websocket");
        if (settings.opt("is_audio_over_websocket_allowed") instanceof Boolean)
        {
            websocket = settings.optBoolean("is_audio_over_websocket_allowed");
        }
        if (settings.opt("is_audio_over_webrtc_allowed") instanceof Boolean)
        {
            webrtc = settings.optBoolean("is_audio_over_webrtc_allowed");
        }
        return websocket ? (webrtc ? "auto" : "websocket") : (webrtc ? "webrtc" : "disabled");
    }
}
