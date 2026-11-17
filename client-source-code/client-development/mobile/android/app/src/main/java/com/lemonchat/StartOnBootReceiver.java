package com.lemonchat;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

/** Receives boot and package update broadcasts. */
public class StartOnBootReceiver extends BroadcastReceiver
{
	/**
	 * Starts the service after a recognized boot or app update broadcast.
	 * @param context receiver context
	 * @param intent received broadcast, possibly {@code null}
	 */
	@Override public void onReceive(Context context, Intent intent)
	{
		String action = (intent != null) ? intent.getAction() : null;

		// Accept boot events and updates to this app.
		if (Intent.ACTION_BOOT_COMPLETED.equals(action) == false
			&& "android.intent.action.QUICKBOOT_POWERON".equals(action) == false
			&& "android.intent.action.LOCKED_BOOT_COMPLETED".equals(action) == false
			&& Intent.ACTION_MY_PACKAGE_REPLACED.equals(action) == false)
		{
			return;
		}

		try
		{
			android.os.UserManager users = (android.os.UserManager)context.getSystemService(Context.USER_SERVICE);
			if (users == null || !users.isUserUnlocked())
			{
				return;
			}
			// Start Node without opening a screen.
			Intent serviceIntent = new Intent(context, BackgroundService.class);

			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
			{
				context.startForegroundService(serviceIntent);
			}
			else
			{
				context.startService(serviceIntent);
			}

			Log.i("lemonchat", "boot: background service started");
		}
		catch (Exception bootStartFailed)
		{
			// A refused start must not crash the receiver.
			Log.w("lemonchat", "boot: could not start the service", bootStartFailed);
		}
	}
}
