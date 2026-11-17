package com.lemonchat.model;

import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import org.json.JSONException;
import org.json.JSONObject;

/** immutable stored permissions, powers and editing thresholds */
public final class PermissionDefinition
{
    public static final class Flags
    {
        public final boolean skip;
        public final boolean negate;

        public Flags(boolean skip, boolean negate)
        {
            this.skip = skip;
            this.negate = negate;
        }
    }

    public final Map<String, String> actions;
    public final Map<String, Integer> powers;
    public final Map<String, Integer> grants;
    public final Map<String, Flags> flags;

    public PermissionDefinition(Map<String, String> actions, Map<String, Integer> powers, Map<String, Integer> grants, Map<String, Flags> flags)
    {
        if (actions.size() + powers.size() > 128 || grants.size() > 128 || flags.size() > 128)
        {
            throw new IllegalArgumentException("Too many permission fields");
        }
        for (Map.Entry<String, String> entry : actions.entrySet())
        {
            name(entry.getKey());
            if (!"grant".equals(entry.getValue()) && !"deny".equals(entry.getValue()) && !"default".equals(entry.getValue()))
            {
                throw new IllegalArgumentException("Invalid permission decision");
            }
        }
        for (Map.Entry<String, Integer> entry : powers.entrySet())
        {
            name(entry.getKey());
            number(entry.getValue());
            if (actions.containsKey(entry.getKey()))
            {
                throw new IllegalArgumentException("Duplicate permission field");
            }
        }
        for (Map.Entry<String, Integer> entry : grants.entrySet())
        {
            known(entry.getKey(), actions, powers);
            number(entry.getValue());
        }
        for (Map.Entry<String, Flags> entry : flags.entrySet())
        {
            known(entry.getKey(), actions, powers);
            if (entry.getValue() == null)
            {
                throw new IllegalArgumentException("Missing permission flags");
            }
        }
        this.actions = Collections.unmodifiableMap(new LinkedHashMap<>(actions));
        this.powers = Collections.unmodifiableMap(new LinkedHashMap<>(powers));
        this.grants = Collections.unmodifiableMap(new LinkedHashMap<>(grants));
        this.flags = Collections.unmodifiableMap(new LinkedHashMap<>(flags));
    }

    public static PermissionDefinition fromJson(JSONObject source) throws JSONException
    {
        if (source.length() > 130 || source.optJSONObject("grants") != null && source.optJSONObject("grants").length() > 128 || source.optJSONObject("flags") != null && source.optJSONObject("flags").length() > 128)
        {
            throw new JSONException("Too many permission fields");
        }
        Map<String, String> actions = new LinkedHashMap<>();
        Map<String, Integer> powers = new LinkedHashMap<>();
        Map<String, Integer> grants = new LinkedHashMap<>();
        Map<String, Flags> flags = new LinkedHashMap<>();
        Iterator<String> names = source.keys();
        while (names.hasNext())
        {
            String key = names.next();
            if (key.equals("grants") || key.equals("flags"))
            {
                continue;
            }
            Object value = source.get(key);
            if (value instanceof String)
            {
                actions.put(key, (String)value);
            }
            else
            {
                powers.put(key, readNumber(value));
            }
        }
        JSONObject thresholds = source.optJSONObject("grants");
        if (source.has("grants") && thresholds == null)
        {
            throw new JSONException("Invalid permission grants");
        }
        if (thresholds != null)
        {
            names = thresholds.keys();
            while (names.hasNext())
            {
                String key = names.next();
                grants.put(key, readNumber(thresholds.get(key)));
            }
        }
        JSONObject options = source.optJSONObject("flags");
        if (source.has("flags") && options == null)
        {
            throw new JSONException("Invalid permission flags");
        }
        if (options != null)
        {
            names = options.keys();
            while (names.hasNext())
            {
                String key = names.next();
                JSONObject value = options.getJSONObject(key);
                Iterator<String> optionsNames = value.keys();
                while (optionsNames.hasNext())
                {
                    String option = optionsNames.next();
                    if ((!option.equals("skip") && !option.equals("negate")) || !(value.get(option) instanceof Boolean))
                    {
                        throw new JSONException("Invalid permission flag");
                    }
                }
                flags.put(key, new Flags(value.optBoolean("skip"), value.optBoolean("negate")));
            }
        }
        try
        {
            return new PermissionDefinition(actions, powers, grants, flags);
        }
        catch (IllegalArgumentException failure)
        {
            throw new JSONException(failure.getMessage());
        }
    }

    /** @return a separate JSON object containing the complete definition */
    public JSONObject toJson()
    {
        JSONObject result = new JSONObject();
        try
        {
            for (Map.Entry<String, String> entry : actions.entrySet())
            {
                result.put(entry.getKey(), entry.getValue());
            }
            for (Map.Entry<String, Integer> entry : powers.entrySet())
            {
                result.put(entry.getKey(), entry.getValue());
            }
            result.put("grants", new JSONObject(grants));
            JSONObject options = new JSONObject();
            for (Map.Entry<String, Flags> entry : flags.entrySet())
            {
                JSONObject value = new JSONObject();
                if (entry.getValue().skip)
                {
                    value.put("skip", true);
                }
                if (entry.getValue().negate)
                {
                    value.put("negate", true);
                }
                if (value.length() > 0)
                {
                    options.put(entry.getKey(), value);
                }
            }
            result.put("flags", options);
            return result;
        }
        catch (JSONException impossible)
        {
            throw new IllegalStateException(impossible);
        }
    }

    public boolean sameValues(PermissionDefinition other)
    {
        if (other == null || !actions.equals(other.actions) || !powers.equals(other.powers))
        {
            return false;
        }
        for (String key : actions.keySet())
        {
            if (!sameOptions(key, other))
            {
                return false;
            }
        }
        for (String key : powers.keySet())
        {
            if (!sameOptions(key, other))
            {
                return false;
            }
        }
        return true;
    }

    private boolean sameOptions(String key, PermissionDefinition other)
    {
        Flags left = flags.get(key);
        Flags right = other.flags.get(key);
        return grants.getOrDefault(key, 1000).equals(other.grants.getOrDefault(key, 1000)) && (left != null && left.skip) == (right != null && right.skip) && (left != null && left.negate) == (right != null && right.negate);
    }

    private static int readNumber(Object value) throws JSONException
    {
        if (!(value instanceof Number) || ((Number)value).doubleValue() != ((Number)value).intValue() || ((Number)value).intValue() < 0 || ((Number)value).intValue() > 1000)
        {
            throw new JSONException("Permission power must be a whole number from 0 to 1000");
        }
        return ((Number)value).intValue();
    }

    private static void number(Integer value)
    {
        if (value == null || value < 0 || value > 1000)
        {
            throw new IllegalArgumentException("Permission power must be from 0 to 1000");
        }
    }

    private static void name(String value)
    {
        if (value == null || !value.matches("[a-z][a-z0-9_]{0,99}") || value.equals("grants") || value.equals("flags"))
        {
            throw new IllegalArgumentException("Invalid permission name");
        }
    }

    private static void known(String key, Map<String, String> actions, Map<String, Integer> powers)
    {
        if (!actions.containsKey(key) && !powers.containsKey(key))
        {
            throw new IllegalArgumentException("Unknown permission option");
        }
    }
}
