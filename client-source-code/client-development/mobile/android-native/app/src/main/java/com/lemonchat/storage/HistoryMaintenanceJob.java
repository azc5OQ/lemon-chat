package com.lemonchat.storage;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import com.lemonchat.service.ChatService;

/** removes expired rows when Android permits background maintenance */
public final class HistoryMaintenanceJob extends JobService
{
    private static final int JOB_ID = 0x4c4849;
    private JobParameters current;

    /**
     * schedules expiry without starting a connection or recording audio
     *
     * @param context application context
     * @param policy explicit history choice
     */
    public static void schedule(Context context, HistoryPolicy policy)
    {
        JobScheduler scheduler = context.getSystemService(JobScheduler.class);
        if (scheduler == null)
        {
            return;
        }
        try
        {
            if (!policy.enabled() || policy.retentionDays <= 0)
            {
                scheduler.cancel(JOB_ID);
                return;
            }
            JobInfo job = new JobInfo.Builder(JOB_ID, new ComponentName(context, HistoryMaintenanceJob.class)).setPersisted(true).setPeriodic(3600000, 900000).build();
            if (scheduler.schedule(job) != JobScheduler.RESULT_SUCCESS)
            {
                android.util.Log.w("LemonNative", "Android deferred history maintenance");
            }
        }
        catch (RuntimeException error)
        {
            android.util.Log.w("LemonNative", "Android refused history maintenance", error);
        }
    }

    @Override
    public boolean onStartJob(JobParameters parameters)
    {
        current = parameters;
        ChatService.maintainSavedHistory(getApplicationContext(), () ->
        {
            if (current == parameters)
            {
                current = null;
                jobFinished(parameters, false);
            }
        });
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters parameters)
    {
        if (current == parameters)
        {
            current = null;
        }
        return false;
    }
}
