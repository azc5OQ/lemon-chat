package com.lemonchat;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.content.Intent;
import android.util.Log;

/** Checks the service through the job registered by {@link BackgroundService}. */
public class RestartJobService extends JobService
{
	/**
	 * Starts the service if its running flag is clear.
	 * @param parameters scheduled job
	 * @return {@code false} because the check is complete
	 */
	@Override public boolean onStartJob(JobParameters parameters)
	{
		if (BackgroundService.isRunning == false)
		{
			try
			{
				this.startForegroundService(new Intent(this, BackgroundService.class));
				Log.i("lemonchat", "restart job: service was not running, starting it");
			}
			catch (Exception startRefused)
			{
				// Android can refuse a service start from a job.
				Log.w("lemonchat", "restart job: start refused", startRefused);
			}
		}

		return false; // The job has no remaining work.
	}

	/**
	 * Leaves later checks to the existing periodic schedule.
	 * @param parameters stopped job
	 * @return {@code false} to avoid a separate retry
	 */
	@Override public boolean onStopJob(JobParameters parameters)
	{
		return false;
	}
}
