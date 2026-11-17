package com.lemonchat;

import java.net.IDN;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import org.json.JSONArray;
import org.json.JSONObject;

/** keeps unfinished connection details outside saved activity state */
final class ConnectionDraft
{
    int step;
    String name;
    String host;
    String port;
    final ArrayList<Key> keys = new ArrayList<>();

    static final class Key
    {
        final Object original;
        String type;
        String password;

        Key(Object original)
        {
            this.original = original;
            type = MetadataKey.type(original);
            password = MetadataKey.password(original);
        }
    }

    ConnectionDraft(JSONObject settings)
    {
        name = settings.optString("default_username", "");
        host = settings.optString("host", "");
        int savedPort = settings.optInt("port", 0);
        port = Integer.toString(savedPort > 0 ? savedPort : 1111);
        JSONArray saved = settings.optJSONArray("metadata_keys");
        if (saved != null)
        {
            for (int index = 0; index < saved.length(); index++)
            {
                keys.add(new Key(saved.opt(index)));
            }
        }
    }

    /**
     * validates one page before moving forward
     *
     * @param page page index from zero
     * @return message to show or null when valid
     */
    String validate(int page)
    {
        if (page == 0)
        {
            if (name.trim().isEmpty())
            {
                return "Enter the name people should see.";
            }
            if (name.indexOf('\0') >= 0)
            {
                return "Remove the unsupported character from your name.";
            }
            if (name.getBytes(StandardCharsets.UTF_8).length >= 100)
            {
                return "Choose a shorter name.";
            }
        }
        if (page == 1)
        {
            try
            {
                normalizedHost();
                int value = Integer.parseInt(port.trim());
                if (value < 1 || value > 65535)
                {
                    return "Enter a port between 1 and 65535.";
                }
            }
            catch (NumberFormatException invalid)
            {
                return "Enter a port between 1 and 65535.";
            }
            catch (Exception invalid)
            {
                return "Enter an IP address or hostname, without a link or path.";
            }
        }
        if (page == 2)
        {
            try
            {
                encodedKeys();
            }
            catch (IllegalArgumentException invalid)
            {
                return invalid.getMessage();
            }
        }
        return null;
    }

    /**
     * creates a patch containing only this flow's connection fields
     *
     * @return validated connection settings
     * @throws Exception when a field needs correction
     */
    JSONObject patch() throws Exception
    {
        for (int page = 0; page < 3; page++)
        {
            String error = validate(page);
            if (error != null)
            {
                throw new IllegalArgumentException(error);
            }
        }
        return new JSONObject().put("default_username", name.trim()).put("host", normalizedHost()).put("port", Integer.parseInt(port.trim())).put("metadata_keys", encodedKeys());
    }

    private JSONArray encodedKeys()
    {
        JSONArray result = new JSONArray();
        for (Key key : keys)
        {
            JSONObject encoded = MetadataKey.encode(key.original, key.type, key.password);
            if (encoded != null)
            {
                result.put(encoded);
            }
        }
        return result;
    }

    private String normalizedHost() throws Exception
    {
        String value = host.trim();
        if (value.startsWith("[") && value.endsWith("]"))
        {
            value = value.substring(1, value.length() - 1);
        }
        if (value.isEmpty() || value.contains("/") || value.contains("@") || value.contains("?") || value.contains("#"))
        {
            throw new IllegalArgumentException("invalid address");
        }
        String ascii = value.contains(":") ? value : IDN.toASCII(value, IDN.USE_STD3_ASCII_RULES);
        URI uri = new URI("ws://" + (ascii.contains(":") ? "[" + ascii + "]" : ascii) + ":1111");
        if (uri.getHost() == null)
        {
            throw new IllegalArgumentException("invalid address");
        }
        return ascii;
    }

    void clear()
    {
        for (Key key : keys)
        {
            key.password = "";
        }
        keys.clear();
    }
}
