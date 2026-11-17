package com.lemonchat.server;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public final class SetupDraftTest
{
    private static SetupDraft Complete()
    {
        SetupDraft draft = new SetupDraft();
        draft.admin = "admin fixture";
        draft.confirmation = draft.admin;
        return draft;
    }

    @Test
    public void DefaultsUseExistingServerSettingsAndHashAdminPassword() throws Exception
    {
        SetupDraft draft = Complete();
        JSONObject settings = draft.Settings();
        assertEquals(7, SetupDraft.STEPS);
        assertEquals(1111, settings.getInt("websocket_port"));
        assertEquals(16, settings.getInt("server_slots"));
        assertEquals("auto", ServerSettings.AudioTransport(settings));
        assertTrue(settings.getBoolean("is_voice_chat_active"));
        assertFalse(settings.getBoolean("is_video_streaming_active"));
        assertTrue(settings.getBoolean("is_music_bot_audio_active"));
        assertTrue(settings.getBoolean("is_chat_picture_allowed"));
        assertFalse(settings.getBoolean("is_file_upload_allowed"));
        assertFalse(settings.getBoolean("is_avatar_upload_allowed"));
        assertEquals(HostingSettings.PAGE_NONE, draft.webPage);
        assertEquals(0, settings.getJSONArray("keys").length());
        assertTrue(settings.getString("admin_password").matches("[A-Za-z0-9+/]{43}="));
        assertFalse(settings.toString().contains(draft.admin));
    }

    @Test
    public void AdminStepRejectsMissingMismatchAndOversizedPasswords()
    {
        SetupDraft draft = new SetupDraft();
        assertNull(draft.Problem(0));
        assertNotNull(draft.Problem(1));
        draft.admin = "admin fixture";
        assertNotNull(draft.Problem(1));
        draft.confirmation = draft.admin;
        assertNull(draft.Problem(1));
        draft.admin = "é".repeat(26);
        draft.confirmation = draft.admin;
        assertNotNull(draft.Problem(1));
        draft.admin = "bad\0password";
        draft.confirmation = draft.admin;
        assertNotNull(draft.Problem(1));
    }

    @Test
    public void OptionalJoinPasswordHasAnActualEffect() throws Exception
    {
        SetupDraft draft = Complete();
        draft.join = "friends fixture";
        assertEquals(0, draft.Settings().getJSONArray("keys").length());
        draft.requireJoin = true;
        assertEquals("friends fixture", ServerSettings.JoinPassword(draft.Settings()));
        draft.join = "";
        assertNotNull(draft.Problem(2));
        draft.join = " trailing ";
        assertNotNull(draft.Problem(2));
        draft.join = "é".repeat(128);
        assertNotNull(draft.Problem(2));
        draft.join = "p".repeat(255);
        assertNull(draft.Problem(2));
    }

    @Test
    public void EverySupportedJoinCipherIsSavedExplicitlyAndSurvivesLaterSettings() throws Exception
    {
        SetupDraft draft = Complete();
        assertEquals("aes256", draft.joinCipher);
        draft.requireJoin = true;
        draft.join = "friends fixture";
        String[] supported = {"aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"};
        assertEquals(supported.length, SetupDraft.JOIN_CIPHERS.size());
        for (String cipher : supported)
        {
            assertTrue(SetupDraft.JOIN_CIPHERS.contains(cipher));
            draft.joinCipher = cipher;
            assertNull(draft.Problem(2));
            JSONObject settings = draft.Settings();
            assertEquals(1, settings.getJSONArray("keys").length());
            JSONObject key = settings.getJSONArray("keys").getJSONObject(0);
            assertEquals(cipher, key.getString("type"));
            assertEquals("friends fixture", key.getString("password"));
            JSONObject changed = ServerSettings.Update(settings, 2222, 32, "", draft.join, false, "webrtc");
            assertEquals(settings.getJSONArray("keys").toString(), changed.getJSONArray("keys").toString());
            assertEquals(settings.getString("admin_password"), changed.getString("admin_password"));
            assertEquals(cipher, draft.Copy().joinCipher);
        }
    }

    @Test
    public void UnknownJoinCipherCannotReachSettings() throws Exception
    {
        SetupDraft draft = Complete();
        draft.requireJoin = true;
        draft.join = "friends fixture";
        for (String cipher : new String[]{null, "", "aes", "AES256", "unknown"})
        {
            draft.joinCipher = cipher;
            assertEquals("Choose a supported password type.", draft.Problem(2));
            try
            {
                draft.Settings();
                fail("unsupported connection ciphers must be refused");
            }
            catch (IllegalArgumentException expected)
            {
                assertEquals("Choose a supported password type.", expected.getMessage());
            }
        }
        draft.joinCipher = "twofish256";
        draft.requireJoin = false;
        assertEquals(0, draft.Settings().getJSONArray("keys").length());
    }

    @Test
    public void CapacityAndAdvancedFieldsMatchServerLimits() throws Exception
    {
        SetupDraft draft = Complete();
        for (String value : new String[]{"", "0", "100001", "1.5", "-1", "99999999999"})
        {
            draft.slots = value;
            assertNotNull(value, draft.Problem(3));
        }
        draft.slots = "100000";
        assertNull(draft.Problem(3));
        for (String value : new String[]{"", "1023", "65536", "1111.5", "-1"})
        {
            draft.port = value;
            assertNotNull(value, draft.Problem(SetupDraft.STEP_REVIEW));
        }
        draft.port = "2222";
        draft.transport = "disabled";
        draft.features.SetEnabled("is_video_streaming_active", true);
        JSONObject settings = draft.Settings();
        assertEquals(2222, settings.getInt("websocket_port"));
        assertEquals(100000, settings.getInt("server_slots"));
        assertFalse(settings.getBoolean("is_audio_over_websocket_allowed"));
        assertFalse(settings.getBoolean("is_audio_over_webrtc_allowed"));
        assertTrue(settings.getBoolean("is_video_streaming_active"));
        draft.transport = "unknown";
        assertNotNull(draft.Problem(SetupDraft.STEP_REVIEW));
    }

    @Test
    public void EveryFeatureSwitchReachesTheSavedSettings() throws Exception
    {
        for (String key : ServerFeatures.KEYS)
        {
            for (boolean isEnabled : new boolean[]{true, false})
            {
                SetupDraft draft = Complete();
                draft.features.SetEnabled(key, isEnabled);
                assertEquals(key, isEnabled, draft.Settings().getBoolean(key));
                assertEquals(key, isEnabled, draft.Copy().features.IsEnabled(key));
            }
        }
    }

    @Test
    public void FeaturesOfSavedSettingsUseTheServerDefaultsForMissingKeys() throws Exception
    {
        JSONObject saved = new JSONObject().put("is_avatar_upload_allowed", true).put("is_music_bot_audio_active", false);
        ServerFeatures features = ServerFeatures.FromSettings(saved);
        assertTrue(features.IsEnabled("is_avatar_upload_allowed"));
        assertFalse(features.IsEnabled("is_music_bot_audio_active"));
        assertTrue(features.IsEnabled("is_voice_chat_active"));
        assertFalse(features.IsEnabled("is_file_upload_allowed"));
    }

    @Test
    public void WebPageStepChecksTheChoiceAndThePort()
    {
        SetupDraft draft = Complete();
        assertNull(draft.Problem(SetupDraft.STEP_WEB_PAGE));
        draft.webPage = "ftp";
        assertNotNull(draft.Problem(SetupDraft.STEP_WEB_PAGE));
        draft.webPage = HostingSettings.PAGE_HTTP;
        for (String value : new String[]{"", "80", "1023", "65536", "8080.5", "-1"})
        {
            draft.webPagePort = value;
            assertNotNull(value, draft.Problem(SetupDraft.STEP_WEB_PAGE));
        }
        draft.webPagePort = draft.port;
        assertNotNull(draft.Problem(SetupDraft.STEP_WEB_PAGE));
        draft.webPagePort = "8080";
        assertNull(draft.Problem(SetupDraft.STEP_WEB_PAGE));
        draft.webPage = HostingSettings.PAGE_FUNNEL;
        draft.webPagePort = "";
        assertNull(draft.Problem(SetupDraft.STEP_WEB_PAGE));
        SetupDraft copy = draft.Copy();
        assertEquals(HostingSettings.PAGE_FUNNEL, copy.webPage);
        assertEquals(draft.isPageConfigEmbedded, copy.isPageConfigEmbedded);
    }

    @Test
    public void EveryCapacityPresetIncluding64IsSavedWithoutClamping() throws Exception
    {
        assertEquals(java.util.Arrays.asList(8, 16, 32, 64), SetupDraft.SLOT_PRESETS);
        SetupDraft draft = Complete();
        for (int count : SetupDraft.SLOT_PRESETS)
        {
            draft.slots = Integer.toString(count);
            assertNull(draft.Problem(3));
            assertEquals(count, draft.Settings().getInt("server_slots"));
        }
    }

    @Test
    public void Custom500SurvivesNavigationAndWorkerCopy() throws Exception
    {
        SetupDraft draft = Complete();
        draft.step = 3;
        draft.slots = "500";
        assertNull(draft.Problem(3));
        draft.step = 4;
        SetupDraft retained = draft.Copy();
        retained.step--;
        assertEquals("500", retained.slots);
        assertNull(retained.Problem(retained.step));
        retained.step++;
        assertEquals(500, retained.Settings().getInt("server_slots"));
        draft.slots = "64";
        assertEquals(500, retained.Settings().getInt("server_slots"));
    }

    @Test
    public void CustomCapacityRejectsInvalidValuesBeforeSaving() throws Exception
    {
        SetupDraft draft = Complete();
        for (String value : new String[]{"", "0", "100001", "-1", "1.5", "500 ", "five hundred", "2147483648"})
        {
            draft.slots = value;
            assertEquals("Enter a whole number of people from 1 to 100,000.", draft.Problem(3));
            try
            {
                draft.Settings();
                fail("invalid capacity must not reach saved settings");
            }
            catch (IllegalArgumentException expected)
            {
                assertEquals(draft.Problem(3), expected.getMessage());
            }
        }
        for (String value : new String[]{"1", "500", "100000"})
        {
            draft.slots = value;
            assertNull(draft.Problem(3));
            assertEquals(Integer.parseInt(value), draft.Settings().getInt("server_slots"));
        }
    }

    @Test
    public void WorkerCopyKeepsTheConfirmedValues()
    {
        SetupDraft draft = Complete();
        draft.requireJoin = true;
        draft.join = "friends fixture";
        draft.joinCipher = "serpent256";
        draft.step = 4;
        draft.advanced = true;
        SetupDraft copy = draft.Copy();
        draft.ClearPasswords();
        draft.slots = "32";
        assertEquals("admin fixture", copy.admin);
        assertEquals(copy.admin, copy.confirmation);
        assertEquals("friends fixture", copy.join);
        assertEquals("serpent256", copy.joinCipher);
        assertEquals("16", copy.slots);
        assertEquals(4, copy.step);
        assertTrue(copy.requireJoin && copy.advanced);
        copy.ClearPasswords();
        assertEquals("", copy.admin);
        assertEquals("", copy.confirmation);
        assertEquals("", copy.join);
    }

    @Test
    public void NewSetupCannotOverwritePrimaryOrRecoveryFiles() throws Exception
    {
        File directory = Files.createTempDirectory("lemon-first-setup-").toFile();
        byte[] original = "existing imported configuration and snapshot reference".getBytes(StandardCharsets.UTF_8);
        try
        {
            assertFalse(ServerSettings.Exists(directory));
            for (String suffix : new String[]{"", ".bak", ".new"})
            {
                File existing = new File(directory, "server_settings.json" + suffix);
                Files.write(existing.toPath(), original);
                assertTrue(ServerSettings.Exists(directory));
                try
                {
                    SetupDraft custom = Complete();
                    custom.slots = "500";
                    ServerSettings.Create(directory, custom);
                    fail("first setup must not replace existing files");
                }
                catch (IOException expected)
                {
                    assertTrue(expected.getMessage().contains("kept"));
                }
                assertArrayEquals(original, Files.readAllBytes(existing.toPath()));
                assertEquals(1, directory.list().length);
                Files.delete(existing.toPath());
            }
        }
        finally
        {
            File[] remaining = directory.listFiles();
            if (remaining != null)
            {
                for (File file : remaining)
                {
                    Files.deleteIfExists(file.toPath());
                }
            }
            Files.deleteIfExists(directory.toPath());
        }
    }
}
