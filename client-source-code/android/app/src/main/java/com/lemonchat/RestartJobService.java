package com.lemonchat;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.content.Intent;
import android.util.Log;

// the second autostart path: the system runs this a few minutes after boot and about every
// 15 minutes, restarting the service if it is not running (registered in BackgroundService)
public class RestartJobService extends JobService
{
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
				// android 12+ may refuse a foreground start from a job; the boot receiver
				// stays the allowed path there
				Log.w("lemonchat", "restart job: start refused", startRefused);
			}
		}

		return false; //nothing keeps running, the check above was the whole job
	}

	@Override public boolean onStopJob(JobParameters parameters)
	{
		return false;
	}
}
