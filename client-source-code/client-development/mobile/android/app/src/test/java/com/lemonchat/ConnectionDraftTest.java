package com.lemonchat;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public final class ConnectionDraftTest
{
    @Test
    public void FreshSetupUsesTheServerDefaultAndKeepsSavedPorts() throws Exception
    {
        assertEquals("1111", new ConnectionDraft(new JSONObject()).port);
        assertEquals("51908", new ConnectionDraft(new JSONObject().put("port", 51908)).port);
    }

    @Test
    public void DraftEditsDoNotChangeSavedSettingsOrIdentity() throws Exception
    {
        JSONObject saved = new JSONObject().put("identity_string", "existing identity").put("rsa_key_bits", 4096).put("host", "old.example").put("port", 1111).put("is_autoconnect_enabled", false).put("default_username", "Alex");
        String before = saved.toString();
        ConnectionDraft draft = new ConnectionDraft(saved);
        draft.host = "new.example";
        draft.name = "Maya";
        JSONObject patch = draft.patch();
        assertEquals(before, saved.toString());
        assertEquals(4, patch.length());
        assertFalse(patch.has("identity_string"));
        assertFalse(patch.has("rsa_key_bits"));
        assertFalse(patch.has("is_autoconnect_enabled"));
        assertEquals("new.example", patch.getString("host"));
    }

    @Test
    public void PlainHostsAndIpv6WorkWithoutProtocolChoices() throws Exception
    {
        for (String address : new String[]{"192.168.1.20", "chat.example.com", "localhost", "::1", "[2001:db8::1]"})
        {
            ConnectionDraft draft = valid();
            draft.host = address;
            assertNull(address, draft.validate(1));
            assertFalse(draft.patch().getString("host").contains("["));
        }
        for (String address : new String[]{"", "ws://server", "server/path", "user@server", "server?query", "bad host", "server:9999"})
        {
            ConnectionDraft draft = valid();
            draft.host = address;
            assertNotNull(address, draft.validate(1));
        }
    }

    @Test
    public void InvalidNameAndPortCannotReachTheFinalPatch() throws Exception
    {
        ConnectionDraft draft = valid();
        draft.name = " ";
        assertNotNull(draft.validate(0));
        draft.name = "é".repeat(50);
        assertNotNull(draft.validate(0));
        draft.name = "Alex";
        for (String port : new String[]{"0", "65536", "abc", ""})
        {
            draft.port = port;
            assertNotNull(draft.validate(1));
            try
            {
                draft.patch();
                fail("invalid port must not produce saved settings");
            }
            catch (IllegalArgumentException expected)
            {
                assertTrue(expected.getMessage().contains("port"));
            }
        }
    }

    @Test
    public void OrderedTypedPasswordsSurviveTheFlow() throws Exception
    {
        JSONArray keys = new JSONArray().put("legacy").put(new JSONObject().put("type", "twofish256").put("password", " exact "));
        ConnectionDraft draft = new ConnectionDraft(new JSONObject().put("default_username", "Alex").put("host", "example.com").put("metadata_keys", keys));
        ConnectionDraft.Key last = new ConnectionDraft.Key(null);
        last.type = "trivium80";
        last.password = "third";
        draft.keys.add(last);
        JSONArray result = draft.patch().getJSONArray("metadata_keys");
        assertEquals("aes256", result.getJSONObject(0).getString("type"));
        assertEquals("twofish256", result.getJSONObject(1).getString("type"));
        assertEquals(" exact ", result.getJSONObject(1).getString("password"));
        assertEquals("trivium80", result.getJSONObject(2).getString("type"));
        assertEquals("legacy", keys.getString(0));
        draft.clear();
        assertEquals(0, draft.keys.size());
        assertEquals("", last.password);
    }

    @Test
    public void UnsupportedSavedPasswordNeedsCorrectionBeforeConnect() throws Exception
    {
        ConnectionDraft draft = valid();
        draft.keys.add(new ConnectionDraft.Key(new JSONObject().put("type", "unknown").put("password", "saved")));
        assertNotNull(draft.validate(2));
        draft.keys.get(0).type = "camellia256";
        assertNull(draft.validate(2));
    }

    private static ConnectionDraft valid() throws Exception
    {
        return new ConnectionDraft(new JSONObject().put("default_username", "Alex").put("host", "localhost"));
    }
}
