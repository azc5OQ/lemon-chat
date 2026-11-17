package com.lemonchat;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public final class MetadataKeyTest
{
    @Test
    public void EverySupportedTypeRoundTripsWithoutChangingTheOriginal() throws Exception
    {
        String[] supported = {"aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"};
        assertEquals(supported.length, MetadataKey.TYPES.size());
        for (String type : supported)
        {
            JSONObject original = new JSONObject().put("type", type).put("password", "saved password").put("future_field", 7);
            assertEquals(type, MetadataKey.type(original));
            assertEquals("saved password", MetadataKey.password(original));
            JSONObject edited = MetadataKey.encode(original, type, "edited password");
            assertEquals(type, edited.getString("type"));
            assertEquals("edited password", edited.getString("password"));
            assertEquals(7, edited.getInt("future_field"));
            assertEquals("saved password", original.getString("password"));
        }
    }

    @Test
    public void LegacyAndNewRowsUseAesWhileTypedLayersKeepTheirOrder() throws Exception
    {
        assertEquals("aes256", MetadataKey.type("legacy password"));
        assertEquals("aes256", MetadataKey.type(null));
        JSONArray rows = new JSONArray();
        rows.put(MetadataKey.encode("legacy password", MetadataKey.type("legacy password"), "legacy password"));
        rows.put(MetadataKey.encode(null, "twofish256", "second password"));
        rows.put(MetadataKey.encode(null, "trivium80", "third password"));
        JSONArray parsed = new JSONArray(rows.toString());
        assertEquals("aes256", parsed.getJSONObject(0).getString("type"));
        assertEquals("legacy password", parsed.getJSONObject(0).getString("password"));
        assertEquals("twofish256", parsed.getJSONObject(1).getString("type"));
        assertEquals("trivium80", parsed.getJSONObject(2).getString("type"));
    }

    @Test
    public void AChangedCipherDoesNotChangeThePasswordOrSavedObject() throws Exception
    {
        JSONObject original = new JSONObject().put("type", "aes256").put("password", " same password ");
        JSONObject edited = MetadataKey.encode(original, "serpent256", MetadataKey.password(original));
        assertEquals("serpent256", edited.getString("type"));
        assertEquals(" same password ", edited.getString("password"));
        assertEquals("aes256", original.getString("type"));
    }

    @Test
    public void UnsupportedSavedTypesAreNotSilentlyChangedToAes() throws Exception
    {
        for (String type : new String[]{"", "AES256", "unknown"})
        {
            JSONObject original = new JSONObject().put("type", type).put("password", "existing password");
            assertEquals(type, MetadataKey.type(original));
            try
            {
                MetadataKey.encode(original, type, MetadataKey.password(original));
                fail("unsupported type must be corrected before saving");
            }
            catch (IllegalArgumentException expected)
            {
                assertTrue(expected.getMessage().contains("supported type"));
            }
            assertEquals(type, original.getString("type"));
        }
    }

    @Test
    public void BlankRowsAreSkippedAndAnExistingEmptyPasswordIsPreserved() throws Exception
    {
        assertNull(MetadataKey.encode(null, "aes256", ""));
        assertNull(MetadataKey.encode("cleared password", "aes256", ""));
        assertEquals("", MetadataKey.encode("", "aes256", "").getString("password"));
        JSONObject empty = new JSONObject().put("type", "camellia256").put("password", "");
        assertEquals("camellia256", MetadataKey.encode(empty, "camellia256", "").getString("type"));
        JSONObject malformed = new JSONObject().put("type", "aes256").put("password", 5);
        try
        {
            MetadataKey.encode(malformed, "aes256", "");
            fail("a malformed saved password must not disappear during Save");
        }
        catch (IllegalArgumentException expected)
        {
            assertTrue(expected.getMessage().contains("invalid"));
        }
        assertEquals("replacement", MetadataKey.encode(malformed, "aes256", "replacement").getString("password"));
    }
}
