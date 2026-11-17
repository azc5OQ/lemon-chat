package com.lemonchat.storage;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.IOException;

/** explicit consent and lifetime for saved private messages */
public final class HistoryPolicy
{
    public static final String PREFERENCES = "history-policy";
    public final boolean configured;
    public final int retentionDays;
    public final boolean erasePending;

    private HistoryPolicy(boolean configured, int retentionDays, boolean erasePending)
    {
        this.configured = configured;
        this.retentionDays = retentionDays;
        this.erasePending = erasePending;
    }

    /**
     * reads consent without creating history or granting access
     *
     * @param context application context
     * @return saved policy or an unconfigured refusal
     */
    public static HistoryPolicy load(Context context)
    {
        return read(context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE));
    }

    static HistoryPolicy read(SharedPreferences preferences)
    {
        Object days = preferences.getAll().get("retention_days");
        Object pending = preferences.getAll().get("erase_pending");
        boolean erase = Boolean.TRUE.equals(pending);
        if (!(days instanceof Integer) || !validDays((Integer)days))
        {
            return new HistoryPolicy(false, -1, erase);
        }
        return new HistoryPolicy(true, (Integer)days, erase);
    }

    public boolean enabled()
    {
        return configured && retentionDays >= 0 && !erasePending;
    }

    public static boolean validDays(int days)
    {
        return days == -1 || days == 0 || days == 1 || days == 7 || days == 30 || days == 90;
    }

    /**
     * commits a user choice or an authorized pending erase
     *
     * @param context application context
     * @param days minus one disables saving, zero removes the age limit
     * @param erase whether deletion must finish before disk access resumes
     * @return committed policy
     * @throws IOException when the decision cannot be saved
     */
    public static HistoryPolicy save(Context context, int days, boolean erase) throws IOException
    {
        if (!validDays(days))
        {
            throw new IllegalArgumentException("Choose a valid history duration");
        }
        SharedPreferences preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        java.util.Map<String, ?> previous = preferences.getAll();
        if (!preferences.edit().putInt("retention_days", days).putBoolean("erase_pending", erase).commit())
        {
            SharedPreferences.Editor rollback = preferences.edit();
            restore(rollback, "retention_days", previous.get("retention_days"));
            restore(rollback, "erase_pending", previous.get("erase_pending"));
            rollback.commit();
            throw new IOException("Your history preference could not be saved");
        }
        return new HistoryPolicy(true, days, erase);
    }

    private static void restore(SharedPreferences.Editor editor, String name, Object value)
    {
        if (value instanceof Integer)
        {
            editor.putInt(name, (Integer)value);
        }
        else if (value instanceof Boolean)
        {
            editor.putBoolean(name, (Boolean)value);
        }
        else if (value instanceof String)
        {
            editor.putString(name, (String)value);
        }
        else if (value instanceof Long)
        {
            editor.putLong(name, (Long)value);
        }
        else if (value instanceof Float)
        {
            editor.putFloat(name, (Float)value);
        }
        else if (value instanceof java.util.Set)
        {
            @SuppressWarnings("unchecked")
            java.util.Set<String> values = (java.util.Set<String>)value;
            editor.putStringSet(name, values);
        }
        else
        {
            editor.remove(name);
        }
    }

    public HistoryPolicy suspended()
    {
        return new HistoryPolicy(configured, retentionDays, true);
    }

    /**
     * calculates expiry from this device's receipt time
     *
     * @param now local wall clock in milliseconds
     * @return oldest retained receipt time, or zero without an expiry
     */
    public long cutoff(long now)
    {
        return retentionDays > 0 ? Math.max(0, now - retentionDays * 86400000L) : 0;
    }
}
