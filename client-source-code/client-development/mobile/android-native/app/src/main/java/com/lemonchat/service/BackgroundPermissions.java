package com.lemonchat.service;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;

/** reads Android background access and opens settings only from a visible activity */
public final class BackgroundPermissions
{
    public static final class Status
    {
        public final boolean batteryExempt;
        public final boolean backgroundRestricted;
        public final boolean notificationsAllowed;
        public final boolean fullScreenCallsAllowed;

        private Status(boolean batteryExempt, boolean backgroundRestricted, boolean notificationsAllowed, boolean fullScreenCallsAllowed)
        {
            this.batteryExempt = batteryExempt;
            this.backgroundRestricted = backgroundRestricted;
            this.notificationsAllowed = notificationsAllowed;
            this.fullScreenCallsAllowed = fullScreenCallsAllowed;
        }
    }

    private BackgroundPermissions()
    {
    }

    /**
     * reads actual grants instead of a saved answer to a permission prompt
     *
     * @param context application or activity context
     * @return current Android background and notification access
     */
    public static Status read(Context context)
    {
        PowerManager power = context.getSystemService(PowerManager.class);
        ActivityManager activities = context.getSystemService(ActivityManager.class);
        NotificationManager notifications = context.getSystemService(NotificationManager.class);
        boolean exempt = power != null && power.isIgnoringBatteryOptimizations(context.getPackageName());
        boolean restricted = Build.VERSION.SDK_INT >= 28 && activities != null && activities.isBackgroundRestricted();
        boolean permitted = Build.VERSION.SDK_INT < 33 || context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED;
        boolean alerts = permitted && notifications != null && notifications.areNotificationsEnabled();
        boolean fullScreen = Build.VERSION.SDK_INT < 34 || notifications != null && notifications.canUseFullScreenIntent();
        return new Status(exempt, restricted, alerts, fullScreen);
    }

    /**
     * asks Android to exempt the direct server connection from battery optimization
     *
     * @param activity visible screen whose user chose this action
     * @return whether Android settings opened, not whether access was granted
     */
    public static boolean requestBatteryExemption(Activity activity)
    {
        if (read(activity).batteryExempt)
        {
            return true;
        }
        Intent request = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + activity.getPackageName()));
        return launch(activity, request) || openBatterySettings(activity);
    }

    /**
     * opens the system battery optimization list when direct exemption is unavailable
     *
     * @param activity visible settings screen
     * @return whether settings opened
     */
    public static boolean openBatterySettings(Activity activity)
    {
        return launch(activity, new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) || openAppSettings(activity);
    }

    /**
     * opens app details for background restrictions and device specific settings
     *
     * @param activity visible settings screen
     * @return whether app details opened
     */
    public static boolean openAppSettings(Activity activity)
    {
        return launch(activity, new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + activity.getPackageName())));
    }

    /**
     * opens app notification channels and their current permissions
     *
     * @param activity visible settings screen
     * @return whether settings opened
     */
    public static boolean openNotificationSettings(Activity activity)
    {
        Intent intent = new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, activity.getPackageName());
        return launch(activity, intent) || openAppSettings(activity);
    }

    /**
     * opens the Android access setting for incoming calls on the lock screen
     *
     * @param activity visible settings screen
     * @return whether settings opened
     */
    public static boolean openFullScreenCallSettings(Activity activity)
    {
        if (Build.VERSION.SDK_INT >= 34)
        {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_USE_FULL_SCREEN_INTENT, Uri.parse("package:" + activity.getPackageName()));
            return launch(activity, intent) || openAppSettings(activity);
        }
        return openNotificationSettings(activity);
    }

    private static boolean launch(Activity activity, Intent intent)
    {
        if (activity.isFinishing() || activity.isDestroyed())
        {
            return false;
        }
        try
        {
            activity.startActivity(intent);
            return true;
        }
        catch (RuntimeException unavailable)
        {
            return false;
        }
    }
}
