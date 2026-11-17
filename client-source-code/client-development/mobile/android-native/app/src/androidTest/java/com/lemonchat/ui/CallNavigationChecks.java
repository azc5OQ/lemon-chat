package com.lemonchat.ui;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;

/** checks the installed call screen task configuration */
public final class CallNavigationChecks
{
    private CallNavigationChecks()
    {
    }

    public static int verify(Context context) throws PackageManager.NameNotFoundException
    {
        PackageManager manager = context.getPackageManager();
        ActivityInfo main = manager.getActivityInfo(new ComponentName(context, MainActivity.class), 0);
        ActivityInfo call = manager.getActivityInfo(new ComponentName(context, IncomingCallActivity.class), 0);
        require(call.taskAffinity != null && !call.taskAffinity.equals(main.taskAffinity), "a background call reused the main task affinity");
        require((call.flags & ActivityInfo.FLAG_EXCLUDE_FROM_RECENTS) != 0, "the temporary call task appears in Recents");
        require(!call.exported, "other apps can launch an untrusted call screen");
        require(call.launchMode == ActivityInfo.LAUNCH_SINGLE_TOP, "repeated notification taps can stack the same call screen");
        return 4;
    }

    private static void require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
}
