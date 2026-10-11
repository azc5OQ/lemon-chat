package com.lemonchat.server;

import android.util.AtomicFile;
import android.util.Base64;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/** Uses the existing C server format. No passwords are bundled in the APK. */
final class ServerSettings
{
    static JSONObject Read(File directory) throws Exception
    {
        AtomicFile file = new AtomicFile(new File(directory, "server_settings.json"));
        JSONObject json = new JSONObject(new String(file.readFully(), StandardCharsets.UTF_8));
        int port = json.getInt("websocket_port");
        int slots = json.getInt("server_slots");
        String transport = json.optString("audio_transport", "webrtc");
        if (json.getDouble("websocket_port") != port || json.getDouble("server_slots") != slots || port < 1024 ||
            port > 65535 || slots < 1 || slots > 100000 ||
            !json.getString("admin_password").matches("[A-Za-z0-9+/]{43}=") ||
            !(transport.equals("webrtc") || transport.equals("auto") || transport.equals("websocket")))
        {
            throw new IllegalArgumentException("Invalid server settings.");
        }
        return json;
    }

    static void Save(File directory, int port, int slots, String admin, String connect, boolean video, String transport) throws Exception
    {
        if (port < 1024 || port > 65535)
        {
            throw new IllegalArgumentException("Choose a port from 1024 to 65535.");
        }
        if (slots < 1 || slots > 100000)
        {
            throw new IllegalArgumentException("Slots must be between 1 and 100000.");
        }
        if (!(transport.equals("webrtc") || transport.equals("auto") || transport.equals("websocket")))
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
        if (!connect.equals(connect.trim()))
        {
            throw new IllegalArgumentException("Remove spaces at the beginning or end of the join password.");
        }
        File destination = new File(directory, "server_settings.json");
        JSONObject json = destination.exists() ? Read(directory) : new JSONObject();
        boolean firstRun = !json.has("admin_password");
        if (!admin.isEmpty())
        {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(admin.getBytes(StandardCharsets.UTF_8));
            json.put("admin_password", Base64.encodeToString(digest, Base64.NO_WRAP));
            json.put("admin_password_is_initial", true);
        }
        else if (firstRun)
        {
            throw new IllegalArgumentException("Set an admin password for the first start.");
        }
        json.put("websocket_port", port);
        json.put("server_slots", slots);
        json.put("audio_transport", transport);
        json.put("is_audio_over_websocket_allowed", !transport.equals("webrtc"));
        json.put("is_audio_over_webrtc_allowed", !transport.equals("websocket"));
        // Preserve extra join keys configured outside this UI when the displayed first key is unchanged.
        JSONArray oldKeys = json.optJSONArray("keys");
        String oldConnect = oldKeys == null ? "" : oldKeys.optString(0, "");
        if (!connect.equals(oldConnect) || oldKeys == null)
        {
            json.put("keys", connect.isEmpty() ? new JSONArray() : new JSONArray().put(connect));
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
            json.put("use_stunnel", false);
            json.put("serve_client_http", false);
            json.put("serve_https", false);
            json.put("restart_on_crash", false);
        }
        json.put("is_video_streaming_active", video);
        AtomicFile file = new AtomicFile(destination);
        FileOutputStream output = null;
        try
        {
            output = file.startWrite();
            output.write(json.toString(2).getBytes(StandardCharsets.UTF_8));
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
}
