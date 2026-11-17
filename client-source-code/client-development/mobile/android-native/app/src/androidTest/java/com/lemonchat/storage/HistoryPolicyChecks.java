package com.lemonchat.storage;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.SharedPreferences;
import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** models Android committing memory before a failed preference disk write */
public final class HistoryPolicyChecks
{
    private int checks;

    public static int verify(Context base) throws Exception
    {
        HistoryPolicyChecks suite = new HistoryPolicyChecks();
        suite.failureCases(base);
        return suite.checks;
    }

    private void failureCases(Context base) throws Exception
    {
        Preferences preferences = new Preferences();
        Context context = new ContextWrapper(base)
        {
            @Override
            public SharedPreferences getSharedPreferences(String name, int mode)
            {
                if (!HistoryPolicy.PREFERENCES.equals(name))
                {
                    throw new AssertionError("Unexpected preference access");
                }
                return preferences;
            }
        };
        check(!HistoryPolicy.load(context).configured, "first use has no saved consent");
        preferences.failures = 1;
        reject(context, 7, false);
        check(!HistoryPolicy.load(context).enabled(), "a failed first choice cannot enable saving in memory");
        check(!HistoryPolicy.load(context).configured, "failed consent preserves the initial undecided state");
        check(preferences.disk.isEmpty(), "failed consent does not become durable during rollback");

        HistoryPolicy.save(context, -1, false);
        preferences.failures = 2;
        reject(context, 30, false);
        HistoryPolicy refused = HistoryPolicy.load(context);
        check(refused.configured && refused.retentionDays == -1 && !refused.enabled(), "failed rollback still preserves refusal in memory");
        check(Integer.valueOf(-1).equals(preferences.disk.get("retention_days")), "failed rollback keeps the prior durable refusal");

        HistoryPolicy.save(context, 30, true);
        preferences.failures = 1;
        reject(context, 30, false);
        check(HistoryPolicy.load(context).erasePending && !HistoryPolicy.load(context).enabled(), "failed erase completion leaves archive access blocked");
        check(Boolean.TRUE.equals(preferences.disk.get("erase_pending")), "failed erase completion preserves durable recovery intent");

        preferences.failures = 2;
        reject(context, 30, false);
        check(HistoryPolicy.load(context).erasePending && !HistoryPolicy.load(context).enabled(), "failed erase rollback cannot resume saving");
        check(Boolean.TRUE.equals(preferences.disk.get("erase_pending")), "failed erase rollback keeps disk recovery pending");

        HistoryPolicy completed = HistoryPolicy.save(context, 30, false);
        check(completed.enabled() && HistoryPolicy.load(context).enabled(), "a successful retry enables the committed choice");
        check(Boolean.FALSE.equals(preferences.disk.get("erase_pending")), "successful retry durably clears the recovery marker");

        preferences.failures = 1;
        reject(context, 1, false);
        check(HistoryPolicy.load(context).retentionDays == 30, "failed duration change preserves the previous duration");
        check(Integer.valueOf(30).equals(preferences.disk.get("retention_days")), "failed duration change preserves its durable value");
    }

    private void reject(Context context, int days, boolean erase) throws Exception
    {
        boolean rejected = false;
        try
        {
            HistoryPolicy.save(context, days, erase);
        }
        catch (IOException expected)
        {
            rejected = true;
        }
        check(rejected, "failed disk commit is reported to the caller");
    }

    private void check(boolean condition, String message)
    {
        checks++;
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }

    private static final class Preferences implements SharedPreferences
    {
        final Map<String, Object> memory = new HashMap<>();
        final Map<String, Object> disk = new HashMap<>();
        int failures;

        @Override
        public Map<String, ?> getAll()
        {
            return new HashMap<>(memory);
        }

        @Override
        public String getString(String key, String fallback)
        {
            return memory.containsKey(key) ? (String)memory.get(key) : fallback;
        }

        @Override
        @SuppressWarnings("unchecked")
        public Set<String> getStringSet(String key, Set<String> fallback)
        {
            return memory.containsKey(key) ? new HashSet<>((Set<String>)memory.get(key)) : fallback;
        }

        @Override
        public int getInt(String key, int fallback)
        {
            return memory.containsKey(key) ? (Integer)memory.get(key) : fallback;
        }

        @Override
        public long getLong(String key, long fallback)
        {
            return memory.containsKey(key) ? (Long)memory.get(key) : fallback;
        }

        @Override
        public float getFloat(String key, float fallback)
        {
            return memory.containsKey(key) ? (Float)memory.get(key) : fallback;
        }

        @Override
        public boolean getBoolean(String key, boolean fallback)
        {
            return memory.containsKey(key) ? (Boolean)memory.get(key) : fallback;
        }

        @Override
        public boolean contains(String key)
        {
            return memory.containsKey(key);
        }

        @Override
        public Editor edit()
        {
            return new Change();
        }

        @Override
        public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener)
        {
        }

        @Override
        public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener)
        {
        }

        private final class Change implements Editor
        {
            private final Map<String, Object> values = new HashMap<>();
            private final Set<String> removed = new HashSet<>();
            private boolean clear;

            @Override
            public Editor putString(String key, String value)
            {
                values.put(key, value);
                return this;
            }

            @Override
            public Editor putStringSet(String key, Set<String> value)
            {
                values.put(key, value == null ? null : new HashSet<>(value));
                return this;
            }

            @Override
            public Editor putInt(String key, int value)
            {
                values.put(key, value);
                return this;
            }

            @Override
            public Editor putLong(String key, long value)
            {
                values.put(key, value);
                return this;
            }

            @Override
            public Editor putFloat(String key, float value)
            {
                values.put(key, value);
                return this;
            }

            @Override
            public Editor putBoolean(String key, boolean value)
            {
                values.put(key, value);
                return this;
            }

            @Override
            public Editor remove(String key)
            {
                removed.add(key);
                return this;
            }

            @Override
            public Editor clear()
            {
                clear = true;
                return this;
            }

            @Override
            public boolean commit()
            {
                if (clear)
                {
                    memory.clear();
                }
                for (String key : removed)
                {
                    memory.remove(key);
                }
                for (Map.Entry<String, Object> value : values.entrySet())
                {
                    if (value.getValue() == null)
                    {
                        memory.remove(value.getKey());
                    }
                    else
                    {
                        memory.put(value.getKey(), value.getValue());
                    }
                }
                if (failures > 0)
                {
                    failures--;
                    return false;
                }
                disk.clear();
                disk.putAll(memory);
                return true;
            }

            @Override
            public void apply()
            {
                commit();
            }
        }
    }
}
