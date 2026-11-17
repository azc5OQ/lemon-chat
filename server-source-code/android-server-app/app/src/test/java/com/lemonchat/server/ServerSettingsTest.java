package com.lemonchat.server;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import static org.junit.Assert.*;

public final class ServerSettingsTest
{
    private static JSONObject Existing() throws Exception
    {
        return new JSONObject().put("admin_password", "a".repeat(43) + "=").put("websocket_port", 1111).put("server_slots", 500)
            .put("keys", new JSONArray().put(new JSONObject().put("type", "chacha20").put("password", "first"))
                .put(new JSONObject().put("type", "aes256").put("password", "second")))
            .put("channels", new JSONArray().put(new JSONObject().put("channel_id", 0).put("channel_name", "Root")))
            .put("max_channel_count", 128).put("music_bot_snapshot", "music-bots-" + "a".repeat(32) + ".bin")
            .put("is_audio_over_websocket_allowed", true).put("is_audio_over_webrtc_allowed", false);
    }

    @Test
    public void SettingsReadHonorsItsByteLimit() throws Exception
    {
        byte[] data = new byte[8193];
        assertArrayEquals(data, ServerSettings.ReadBytes(new ByteArrayInputStream(data), data.length));
        try
        {
            ServerSettings.ReadBytes(new ByteArrayInputStream(data), data.length - 1);
            fail("oversized settings must be refused");
        }
        catch (IOException expected)
        {
            assertTrue(expected.getMessage().contains("original file has not been changed"));
        }
    }

    @Test
    public void TypedPasswordsAndUnrelatedStateSurviveSave() throws Exception
    {
        JSONObject existing = Existing();
        String before = existing.toString();
        assertEquals("first", ServerSettings.JoinPassword(existing));
        JSONObject updated = ServerSettings.Update(existing, 2222, 32, "", ServerSettings.JoinPassword(existing), true, ServerSettings.AudioTransport(existing));
        assertEquals(existing.getJSONArray("keys").toString(), updated.getJSONArray("keys").toString());
        assertEquals(existing.getJSONArray("channels").toString(), updated.getJSONArray("channels").toString());
        assertEquals(existing.getString("music_bot_snapshot"), updated.getString("music_bot_snapshot"));
        assertEquals(128, updated.getInt("max_channel_count"));
        assertTrue(updated.getBoolean("is_audio_over_websocket_allowed"));
        assertFalse(updated.getBoolean("is_audio_over_webrtc_allowed"));
        assertEquals(before, existing.toString());
    }

    @Test
    public void ChangingFirstPasswordPreservesItsCipherAndOtherLayers() throws Exception
    {
        JSONObject existing = Existing();
        existing.remove("music_bot_snapshot");
        JSONObject updated = ServerSettings.Update(existing, 1111, 500, "", "changed", false, "websocket");
        JSONArray keys = updated.getJSONArray("keys");
        assertEquals(2, keys.length());
        assertEquals("chacha20", keys.getJSONObject(0).getString("type"));
        assertEquals("changed", keys.getJSONObject(0).getString("password"));
        assertEquals(existing.getJSONArray("keys").getJSONObject(1).toString(), keys.getJSONObject(1).toString());
        assertEquals("first", ServerSettings.JoinPassword(existing));
        JSONObject cleared = ServerSettings.Update(existing, 1111, 500, "", "", false, "websocket");
        assertEquals(0, cleared.getJSONArray("keys").length());
    }

    @Test
    public void PasswordLimitAppliesToPasswordRatherThanTypedJson() throws Exception
    {
        JSONObject existing = Existing();
        existing.getJSONArray("keys").getJSONObject(0).put("password", "p".repeat(255));
        JSONObject updated = ServerSettings.Update(existing, 1111, 500, "", ServerSettings.JoinPassword(existing), false, "websocket");
        assertEquals(255, ServerSettings.JoinPassword(updated).length());
        boolean rejected = false;
        try
        {
            ServerSettings.Update(existing, 1111, 500, "", "p".repeat(256), false, "websocket");
        }
        catch (IllegalArgumentException expected)
        {
            rejected = true;
        }
        assertTrue(rejected);
    }

    @Test
    public void LegacyLayersAndExistingWhitespaceRemainReadable() throws Exception
    {
        JSONObject existing = Existing().put("keys", new JSONArray().put(" existing ").put("second"));
        existing.remove("music_bot_snapshot");
        JSONObject same = ServerSettings.Update(existing, 1111, 500, "", ServerSettings.JoinPassword(existing), false, "auto");
        assertEquals(existing.getJSONArray("keys").toString(), same.getJSONArray("keys").toString());
        JSONObject changed = ServerSettings.Update(existing, 1111, 500, "", "new", false, "auto");
        assertEquals("new", changed.getJSONArray("keys").getString(0));
        assertEquals("second", changed.getJSONArray("keys").getString(1));
    }

    @Test
    public void ExplicitFlagsOverrideLegacyTransportAndPreserveDisabledMode() throws Exception
    {
        for (int flags = 0; flags < 4; flags++)
        {
            boolean websocket = (flags & 1) != 0;
            boolean webrtc = (flags & 2) != 0;
            JSONObject existing = Existing().put("audio_transport", "webrtc").put("is_audio_over_websocket_allowed", websocket).put("is_audio_over_webrtc_allowed", webrtc);
            String mode = ServerSettings.AudioTransport(existing);
            assertEquals(websocket ? (webrtc ? "auto" : "websocket") : (webrtc ? "webrtc" : "disabled"), mode);
            JSONObject updated = ServerSettings.Update(existing, 1111, 500, "", "first", false, mode);
            assertEquals(websocket, updated.getBoolean("is_audio_over_websocket_allowed"));
            assertEquals(webrtc, updated.getBoolean("is_audio_over_webrtc_allowed"));
        }
        assertEquals("auto", ServerSettings.AudioTransport(new JSONObject()));
        assertEquals("websocket", ServerSettings.AudioTransport(new JSONObject().put("audio_transport", "websocket")));
        assertEquals("disabled", ServerSettings.AudioTransport(new JSONObject().put("audio_transport", "webrtc").put("is_audio_over_webrtc_allowed", false)));
    }

    @Test
    public void SavedMusicRefusesOfflinePasswordChanges() throws Exception
    {
        for (String reference : new String[] { "music_bot_snapshot", "music_bot_snapshot_previous" })
        {
            JSONObject existing = Existing();
            String snapshot = existing.getString("music_bot_snapshot");
            existing.remove("music_bot_snapshot");
            existing.put(reference, snapshot);
            for (String password : new String[] { "changed", "" })
            {
                try
                {
                    ServerSettings.Update(existing, 1111, 500, "", password, false, "websocket");
                    fail("saved music must retain its decryption password");
                }
                catch (IllegalArgumentException expected)
                {
                    assertTrue(expected.getMessage().contains("Saved music"));
                }
            }
            JSONObject updated = ServerSettings.Update(existing, 1111, 500, "new admin", "first", false, "websocket");
            assertEquals(snapshot, updated.getString(reference));
            assertEquals("first", ServerSettings.JoinPassword(existing));
        }
    }

    @Test
    public void FirstRunStillUsesTheSelectedModeAndHashesAdminPassword() throws Exception
    {
        JSONObject updated = ServerSettings.Update(new JSONObject(), 1111, 16, "admin fixture", "", false, "webrtc");
        assertTrue(updated.getString("admin_password").matches("[A-Za-z0-9+/]{43}="));
        assertFalse(updated.getBoolean("is_audio_over_websocket_allowed"));
        assertTrue(updated.getBoolean("is_audio_over_webrtc_allowed"));
        assertEquals(0, updated.getJSONArray("keys").length());
        assertFalse(updated.getBoolean("is_music_bot_audio_active"));
    }
}
