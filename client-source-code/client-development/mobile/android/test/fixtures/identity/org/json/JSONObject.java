package org.json;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
public class JSONObject
{
    private static final AtomicInteger next = new AtomicInteger();
    private static final Map<String, Map<String, Object>> snapshots = new ConcurrentHashMap<>();
    private final Map<String, Object> values;
    public JSONObject()
    {
        values = new HashMap<>();
    }
    public JSONObject(String token)
    {
        Map<String, Object> saved = snapshots.get(token);
        if (saved == null)
        {
            throw new IllegalArgumentException("Unknown fixture object token");
        }
        values = new HashMap<>(saved);
    }
    public JSONObject put(String key, Object value)
    {
        values.put(key, value);
        return this;
    }
    public boolean has(String key)
    {
        return values.containsKey(key);
    }
    public String getString(String key)
    {
        return (String)values.get(key);
    }
    public String optString(String key, String fallback)
    {
        Object value = values.get(key);
        return value == null ? fallback : (String)value;
    }
    @Override public String toString()
    {
        String token = "fixture-object-" + next.incrementAndGet();
        snapshots.put(token, new HashMap<>(values));
        return token;
    }
}
