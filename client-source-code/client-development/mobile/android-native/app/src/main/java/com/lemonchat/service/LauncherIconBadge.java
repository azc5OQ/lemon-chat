package com.lemonchat.service;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Shows the number of unread messages on the launcher icon.
 *
 * Android has no interface for drawing a number onto an app icon. The manifest therefore declares 101 launcher
 * entries (activity-alias elements) that all open MainActivity: LauncherDefault with the plain icon and
 * LauncherBadge1 to LauncherBadge99plus with a red badge drawn into the icon. This class enables the entry that
 * matches the count and disables the one enabled before. The icons are written by
 * mobile/tools/generate-launcher-badges.py. The hybrid app (mobile/android) uses the same mechanism.
 */
final class LauncherIconBadge
{
    // index 0 is the plain icon, 1 to 99 that number, 100 is "99+"
    private static final int HIGHEST_INDEX = 100;

    // time without a new count before the icon is switched; every switch makes the launcher reload the app's entries
    private static final long SWITCH_DELAY_MS = 4000;

    private static final Handler main = new Handler(Looper.getMainLooper());
    private static Runnable pendingSwitch;

    // index of the enabled entry; -1 until the first switch of this process, the enabled entry itself is stored by Android
    private static int currentIndex = -1;

    private LauncherIconBadge()
    {
    }

    /**
     * @brief shows a count on the launcher icon after the count has not changed for SWITCH_DELAY_MS
     *
     * @param context any context of the app
     * @param unreadCount number to show; zero or less shows the plain icon, more than 99 shows "99+"
     */
    static void apply(Context context, int unreadCount)
    {
        int targetIndex = unreadCount <= 0 ? 0 : Math.min(unreadCount, HIGHEST_INDEX);

        // skip an unchanged count
        if (targetIndex == currentIndex && pendingSwitch == null)
        {
            return;
        }

        // replace a waiting switch
        if (pendingSwitch != null)
        {
            main.removeCallbacks(pendingSwitch);
        }
        Context application = context.getApplicationContext();
        pendingSwitch = () ->
        {
            pendingSwitch = null;
            switchEntry(application, targetIndex);
        };
        main.postDelayed(pendingSwitch, SWITCH_DELAY_MS);
    }

    /**
     * @brief manifest name of the launcher entry for an index
     *
     * @param index 0 to HIGHEST_INDEX
     *
     * @return class name of the activity-alias
     */
    private static String entryName(int index)
    {
        if (index == 0)
        {
            return "com.lemonchat.LauncherDefault";
        }
        return "com.lemonchat.LauncherBadge" + (index == HIGHEST_INDEX ? "99plus" : String.valueOf(index));
    }

    /**
     * @brief enables the entry of an index and disables every other enabled entry
     *
     * @param context application context
     * @param targetIndex 0 to HIGHEST_INDEX
     */
    private static void switchEntry(Context context, int targetIndex)
    {
        if (targetIndex == currentIndex)
        {
            return;
        }
        try
        {
            PackageManager packages = context.getPackageManager();

            // enable the new entry first
            packages.setComponentEnabledSetting(new ComponentName(context, entryName(targetIndex)), PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);

            // disable the previous entry
            if (currentIndex >= 0)
            {
                packages.setComponentEnabledSetting(new ComponentName(context, entryName(currentIndex)), PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
            }
            else
            {
                // find entries enabled by an earlier process
                for (int index = 0; index <= HIGHEST_INDEX; index++)
                {
                    if (index == targetIndex)
                    {
                        continue;
                    }
                    ComponentName entry = new ComponentName(context, entryName(index));
                    int state = packages.getComponentEnabledSetting(entry);

                    // only the plain entry is enabled by the manifest itself
                    boolean isEnabled = state == PackageManager.COMPONENT_ENABLED_STATE_ENABLED || (state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && index == 0);
                    if (isEnabled)
                    {
                        packages.setComponentEnabledSetting(entry, PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
                    }
                }
            }
            currentIndex = targetIndex;
        }
        catch (RuntimeException error)
        {
            Log.w("LemonNative", "Launcher icon switch failed", error);
        }
    }
}
