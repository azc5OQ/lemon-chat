package com.lemonchat.service;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;

/** retries a previously requested connection if Android later permits a service restart */
public final class ConnectionRestartJob extends JobService
{
    public static final String ACTION_RECOVER = "com.lemonchat.RECOVER_CONNECTION";
    private static final int JOB_ID = 1001;

    /**
     * keeps one persisted recovery job while the user wants the saved connection
     *
     * @param context service or application context
     * @return whether scheduling is available or recovery was not requested
     */
    public static boolean schedule(Context context)
    {
        if (!allowsRecovery(context.getSharedPreferences("preferences", MODE_PRIVATE)))
        {
            cancel(context);
            return true;
        }
        try
        {
            JobScheduler scheduler = context.getSystemService(JobScheduler.class);
            if (scheduler == null)
            {
                return false;
            }
            if (scheduler.getPendingJob(JOB_ID) != null)
            {
                return true;
            }
            JobInfo job = new JobInfo.Builder(JOB_ID, new ComponentName(context, ConnectionRestartJob.class)).setPeriodic(15 * 60 * 1000L).setPersisted(true).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build();
            return scheduler.schedule(job) == JobScheduler.RESULT_SUCCESS;
        }
        catch (RuntimeException unavailable)
        {
            android.util.Log.w("LemonNative", "Background recovery job is unavailable", unavailable);
            return false;
        }
    }

    /**
     * removes recovery after a manual disconnect or invalid saved connection
     *
     * @param context service or application context
     */
    public static void cancel(Context context)
    {
        try
        {
            JobScheduler scheduler = context.getSystemService(JobScheduler.class);
            if (scheduler != null)
            {
                scheduler.cancel(JOB_ID);
            }
        }
        catch (RuntimeException unavailable)
        {
            android.util.Log.w("LemonNative", "Background recovery job could not be cancelled", unavailable);
        }
    }

    static boolean allowsRecovery(SharedPreferences preferences)
    {
        return preferences.getBoolean("saved_connection_valid", false) && !preferences.getBoolean("reconnect_suppressed", false) && (preferences.getBoolean("connection_requested", false) || preferences.getBoolean("automatic_connection", false));
    }

    /**
     * asks the service to restore only the currently authorized saved connection
     *
     * @param parameters scheduled recovery request
     * @return false because the service owns subsequent connection work
     */
    @Override
    public boolean onStartJob(JobParameters parameters)
    {
        if (!allowsRecovery(getSharedPreferences("preferences", MODE_PRIVATE)))
        {
            cancel(this);
            return false;
        }
        try
        {
            startForegroundService(new Intent(this, ChatService.class).setAction(ACTION_RECOVER));
        }
        catch (RuntimeException refused)
        {
            android.util.Log.w("LemonNative", "Android deferred background connection recovery", refused);
        }
        return false;
    }

    /**
     * leaves retries to the existing periodic job
     *
     * @param parameters interrupted request
     * @return false to avoid another independent retry
     */
    @Override
    public boolean onStopJob(JobParameters parameters)
    {
        return false;
    }
}
