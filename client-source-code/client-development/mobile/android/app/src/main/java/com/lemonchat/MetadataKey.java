package com.lemonchat;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.json.JSONException;
import org.json.JSONObject;

/** keeps each server password paired with its connection cipher */
final class MetadataKey
{
    static final List<String> TYPES = Collections.unmodifiableList(Arrays.asList("aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"));
    static final List<String> LABELS = Collections.unmodifiableList(Arrays.asList("AES-256", "ChaCha20", "Camellia-256", "Twofish-256", "Serpent-256", "Trivium-80"));

    private MetadataKey()
    {
    }

    /**
     * reads a saved cipher and defaults legacy passwords to aes
     *
     * @param original saved entry or null for a new row
     * @return cipher name or an empty string for malformed entries
     */
    static String type(Object original)
    {
        if (original == null || original instanceof String)
        {
            return "aes256";
        }
        Object value = original instanceof JSONObject ? ((JSONObject)original).opt("type") : null;
        return value instanceof String ? (String)value : "";
    }

    /**
     * reads the password without changing spaces or letter case
     *
     * @param original saved entry or null for a new row
     * @return password text or an empty string when absent
     */
    static String password(Object original)
    {
        if (original instanceof String)
        {
            return (String)original;
        }
        Object value = original instanceof JSONObject ? ((JSONObject)original).opt("password") : null;
        return value instanceof String ? (String)value : "";
    }

    /**
     * copies the selected type and password without changing the saved entry
     *
     * @param original saved entry or null for a new row
     * @param type selected cipher name
     * @param password current password text
     * @return typed key or null for a cleared or unused row
     */
    static JSONObject encode(Object original, String type, String password)
    {
        if (!TYPES.contains(type))
        {
            throw new IllegalArgumentException("Choose a supported type for every server key.");
        }
        boolean savedPassword = original instanceof String || (original instanceof JSONObject && ((JSONObject)original).opt("password") instanceof String);
        if (password.isEmpty())
        {
            if (original != null && !savedPassword)
            {
                throw new IllegalArgumentException("A saved server key is invalid. Replace its password or remove that row.");
            }
            if (!savedPassword || !password(original).isEmpty())
            {
                return null;
            }
        }
        try
        {
            JSONObject result = original instanceof JSONObject ? new JSONObject(original.toString()) : new JSONObject();
            return result.put("type", type).put("password", password);
        }
        catch (JSONException invalid)
        {
            throw new IllegalArgumentException("This server key could not be saved.", invalid);
        }
    }
}
