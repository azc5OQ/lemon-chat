package com.lemonchat.protocol;

import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

/** bounded JSON parsing shared by the session and its tests */
public final class ProtocolJson
{
    private ProtocolJson()
    {
    }

    /**
     * parses a complete bounded object
     *
     * @param text JSON object text
     * @param limit maximum character count
     * @return parsed object
     * @throws JSONException when the object is invalid or exceeds a limit
     */
    public static JSONObject object(String text, int limit) throws JSONException
    {
        if (text == null || text.isEmpty() || text.length() > limit)
        {
            throw new JSONException("Invalid JSON size");
        }
        int depth = 0;
        boolean quoted = false;
        boolean escaped = false;
        for (int i = 0; i < text.length(); i++)
        {
            char value = text.charAt(i);
            if (value == 0 || (value < 32 && (quoted || !Character.isWhitespace(value))))
            {
                throw new JSONException("Invalid JSON character");
            }
            if (quoted)
            {
                if (escaped)
                {
                    if (value == 'u' && text.startsWith("0000", i + 1))
                    {
                        throw new JSONException("Invalid JSON string");
                    }
                    escaped = false;
                }
                else if (value == '\\')
                {
                    escaped = true;
                }
                else if (value == '"')
                {
                    quoted = false;
                }
            }
            else if (value == '"')
            {
                quoted = true;
            }
            else if (value == '{' || value == '[')
            {
                if (++depth > 64)
                {
                    throw new JSONException("JSON nesting limit exceeded");
                }
            }
            else if (value == '}' || value == ']')
            {
                if (--depth < 0)
                {
                    throw new JSONException("Invalid JSON nesting");
                }
            }
        }
        if (quoted || depth != 0)
        {
            throw new JSONException("Incomplete JSON");
        }
        JSONTokener parser = new JSONTokener(text);
        Object parsed = parser.nextValue();
        if (!(parsed instanceof JSONObject) || parser.nextClean() != 0)
        {
            throw new JSONException("Expected one JSON object");
        }
        return (JSONObject) parsed;
    }

    /**
     * reads an exact bounded integer without coercing strings
     *
     * @param object containing object
     * @param name field name
     * @param minimum smallest accepted value
     * @param maximum largest accepted value
     * @return integer value
     * @throws JSONException when the field is missing or invalid
     */
    public static long integer(JSONObject object, String name, long minimum, long maximum) throws JSONException
    {
        Object value = object.opt(name);
        if (!(value instanceof Number))
        {
            throw new JSONException("Missing numeric " + name);
        }
        double number = ((Number) value).doubleValue();
        if (!Double.isFinite(number) || number != Math.rint(number) || number < minimum || number > maximum)
        {
            throw new JSONException("Invalid numeric " + name);
        }
        return ((Number) value).longValue();
    }

    /**
     * reads a bounded string without coercing other values
     *
     * @param object containing object
     * @param name field name
     * @param limit maximum character count
     * @return string value
     * @throws JSONException when the field is missing or invalid
     */
    public static String string(JSONObject object, String name, int limit) throws JSONException
    {
        Object value = object.opt(name);
        if (!(value instanceof String) || ((String) value).length() > limit || ((String) value).indexOf(0) >= 0)
        {
            throw new JSONException("Invalid string " + name);
        }
        return (String) value;
    }
}
