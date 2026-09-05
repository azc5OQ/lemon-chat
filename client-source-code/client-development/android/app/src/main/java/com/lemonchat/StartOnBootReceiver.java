package com.lemonchat;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

// this receiver is registered in AndroidManifest.xml

public class StartOnBootReceiver extends BroadcastReceiver
{
	@Override public void onReceive(Context context, Intent intent)
	{
		String action = (intent != null) ? intent.getAction() : null;

		// QUICKBOOT_POWERON is the same event under a different name on some devices; it was in
		// the manifest filter but never matched here, so those phones did nothing.
		// MY_PACKAGE_REPLACED restarts the service after every apk update
		if (Intent.ACTION_BOOT_COMPLETED.equals(action) == false
			&& "android.intent.action.QUICKBOOT_POWERON".equals(action) == false
			&& "android.intent.action.LOCKED_BOOT_COMPLETED".equals(action) == false
			&& Intent.ACTION_MY_PACKAGE_REPLACED.equals(action) == false)
		{
			return;
		}

		try
		{
			// start the SERVICE, not MainActivity: since android 10 an app in the background may
			// not launch an activity at all, so the old startActivity here was silently dropped
			// and nothing ever came up on boot. the service stands on its own - it builds the
			// webview in its own context, starts node and calls startForeground itself.
			// BOOT_COMPLETED is one of the cases that may still start a foreground service.
			// node only: no user is looking at a screen on boot, and the whole point of the
			// embedded node runtime is that the connection does not need the webview.
			// the service creates one only when a ui binds, so nothing extra is needed here
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
			// a phone that refuses the start (aggressive battery management) must not crash here
			Log.w("lemonchat", "boot: could not start the service", bootStartFailed);
		}
	}
}
