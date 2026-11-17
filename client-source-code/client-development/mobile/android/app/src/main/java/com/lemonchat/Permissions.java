package com.lemonchat;

import android.Manifest;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.net.Uri;
import android.provider.Settings;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.core.app.NotificationManagerCompat;

/** requests optional connection permissions after an explicit Connect action */
public final class Permissions
{
    public static final int PERMISSIONS_RECORD_AUDIO_REQUEST_CODE = 123;
    private final MainActivity context;
    private final ActivityResultLauncher<Intent> settingsResult;
    private final ActivityResultLauncher<String> notificationResult;
    private boolean notificationRequested;
    private boolean batteryRequested;
    private boolean requestPending;
    private AlertDialog backgroundDialog;

    /**
     * registers callbacks before the activity starts
     *
     * @param context activity owning the connection request
     */
    public Permissions(MainActivity context)
    {
        this.context = context;
        this.settingsResult = context.registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> this.resumeCheck());
        this.notificationResult = context.registerForActivityResult(new ActivityResultContracts.RequestPermission(), granted -> this.resumeCheck());
    }

    /** opens setup without requesting unrelated permissions */
    public void beginCheck()
    {
        this.context.promptForAppModeIfNeeded();
    }

    /**
     * offers notification and background delivery permissions once
     *
     * @return whether the pending Connect action can continue
     */
    public boolean prepareConnection()
    {
        if (this.requestPending || !this.context.isMediaActivityVisible())
        {
            return false;
        }
        if (!this.notificationRequested && !NotificationManagerCompat.from(this.context).areNotificationsEnabled())
        {
            this.notificationRequested = true;
            if (Build.VERSION.SDK_INT >= 33)
            {
                this.requestPending = true;
                try
                {
                    this.notificationResult.launch(Manifest.permission.POST_NOTIFICATIONS);
                    return false;
                }
                catch (RuntimeException unavailable)
                {
                    this.requestPending = false;
                }
            }
        }
        PowerManager power = (PowerManager)this.context.getSystemService(Context.POWER_SERVICE);
        if (!this.batteryRequested && power != null && !power.isIgnoringBatteryOptimizations(this.context.getPackageName()))
        {
            this.batteryRequested = true;
            this.requestPending = true;
            this.offerBackgroundDelivery();
            return false;
        }
        return true;
    }

    /** resumes the original action after permission approval or refusal */
    public void resumeCheck()
    {
        this.requestPending = false;
        this.context.resumePendingConnection();
    }

    /**
     * restores only whether optional prompts have already been offered
     *
     * @param state previous activity state
     */
    public void restoreState(Bundle state)
    {
        if (state != null)
        {
            this.notificationRequested = state.getBoolean("connection_notification_offered");
            this.batteryRequested = state.getBoolean("connection_battery_offered");
        }
    }

    /**
     * preserves prompt decisions without storing connection secrets
     *
     * @param state outgoing activity state
     */
    public void saveState(Bundle state)
    {
        state.putBoolean("connection_notification_offered", this.notificationRequested);
        state.putBoolean("connection_battery_offered", this.batteryRequested);
    }

    /** closes the old activity's explanation after replacement */
    public void destroy()
    {
        if (this.backgroundDialog != null)
        {
            this.backgroundDialog.dismiss();
            this.backgroundDialog = null;
        }
    }

    /** explains the optional battery exemption before opening Android settings */
    private void offerBackgroundDelivery()
    {
        this.backgroundDialog = new AlertDialog.Builder(this.context)
            .setTitle("Receive calls in the background")
            .setMessage("Android may pause the connection when the screen is off. Allowing background activity helps messages and calls arrive. You can connect without changing this setting.")
            .setPositiveButton("Allow background activity", (dialog, which) ->
            {
                try
                {
                    this.settingsResult.launch(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + this.context.getPackageName())));
                }
                catch (RuntimeException unavailable)
                {
                    this.resumeCheck();
                }
            })
            .setNegativeButton("Not now", (dialog, which) -> this.resumeCheck())
            .setOnCancelListener(dialog -> this.resumeCheck())
            .show();
    }
}
