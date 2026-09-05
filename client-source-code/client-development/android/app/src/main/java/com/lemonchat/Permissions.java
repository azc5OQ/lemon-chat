package com.lemonchat;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.core.app.ActivityCompat;
import androidx.core.app.NotificationManagerCompat;
import androidx.core.content.ContextCompat;

public class Permissions
{
	public static final int PERMISSIONS_RECORD_AUDIO_REQUEST_CODE = 123;
	private MainActivity context;

	private ActivityResultLauncher<Intent> requestPermissionLauncher = null;

	private ActivityResultLauncher<String> requestPermissionLauncher1 = null;

	//once-guards for the two DIALOG-based permissions. android auto-denies a repeated request
	//instantly after enough refusals, and doCheck re-runs on every result - without the guard a
	//denial would spin request->auto-deny->request forever and the checks after it would never run
	private boolean notificationPermissionRequestedOnce = false;

	private boolean audioPermissionRequestedOnce = false;

	public Permissions(MainActivity context)
	{
		this.context = context;

		this.requestPermissionLauncher = this.context.registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
			this.doCheck(); //force running doCheck again
		});

		//notification permission request needs ActivityResultContracts.RequestPermission, not  ActivityResultContracts.StartActivityForResult
		this.requestPermissionLauncher1 = this.context.registerForActivityResult(new ActivityResultContracts.RequestPermission(), isGranted -> {
			this.doCheck(); //force running doCheck again
		});
	}

	public void beginCheck()
	{
		this.doCheck();
	}

	//doCheck is invoked over and over again until all permissions are okay
	private void doCheck()
	{
		Log.d("Info", "[lemonchat] PermissionChecker doCheck");

		//notifications off is the thing to fix, whatever the api level. asked at most once, and a
		//refusal must not block the checks below
		if (this.notificationPermissionRequestedOnce == false
			&& NotificationManagerCompat.from(this.context).areNotificationsEnabled() == false)
		{
			this.notificationPermissionRequestedOnce = true;
			this.requestNotificationPermission();
			return;
		}

		// draw-over-other-apps is NO LONGER REQUESTED. it only ever existed so the service could park
		// the webview in a 0x0 overlay window and keep its javascript timers running in the background.
		// the background connection is moving into the embedded node runtime (NodeRuntime), which needs
		// no window at all, so the permission - and the "displaying over other apps" notice that comes
		// with it - is gone. requestDrawingOverOtherAppsPermission() is left in place, unused, so this
		// is one line to put back if the overlay ever has to return.

		//check disable doze mode permission
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
		{
			PowerManager pm = (PowerManager)this.context.getSystemService(Context.POWER_SERVICE);

			if (pm.isIgnoringBatteryOptimizations(this.context.getPackageName()) == false)
			{
				CharSequence text = "so app can run in background ";
				int duration = Toast.LENGTH_LONG;
				Toast toast = Toast.makeText(this.context, text, duration);
				toast.show();

				this.requestDisableBatteryOptimizationsPermission();
				return;
			}
		}

		//check microphone usage permission (asked at most once; resumeCheck re-enters here with the
		//user's answer already given, so a denial falls through to the first-run question below)
		if (ContextCompat.checkSelfPermission(this.context, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED && this.audioPermissionRequestedOnce == false)
		{
			this.audioPermissionRequestedOnce = true;
			this.requestAudioPermission();
			return;
		}

		// every prompt is done: now our own first-run question can have the screen to itself
		this.context.promptForAppModeIfNeeded();
	}

	//re-enters the permission chain after a classic requestPermissions result (the microphone one).
	//MainActivity.onRequestPermissionsResult calls this instead of asking the first-run question
	//directly - asking there fired after the FIRST system prompt, not the last
	public void resumeCheck()
	{
		this.doCheck();
	}

	private void requestNotificationPermission()
	{
		Log.d("Info", "[lemonchat] PermissionChecker requestNotificationPermission");

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
			&& this.context.getApplicationInfo().targetSdkVersion >= Build.VERSION_CODES.TIRAMISU)
		{
			this.requestPermissionLauncher1.launch(Manifest.permission.POST_NOTIFICATIONS);
			return;
		}

		Toast.makeText(this.context, "please allow notifications, calls and messages are announced through them", Toast.LENGTH_LONG).show();

		Intent intent = new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS);
		intent.putExtra(Settings.EXTRA_APP_PACKAGE, this.context.getPackageName());

		//the same launcher the battery-optimisation screen uses, so returning re-enters doCheck
		requestPermissionLauncher.launch(intent);
	}

	private void requestDisableBatteryOptimizationsPermission()
	{
		Log.d("Info", "[lemonchat] PermissionChecker requestDisableBatteryOptimizationsPermission");
		String packageName = this.context.getPackageName();
		Intent intent = new Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + packageName));
		requestPermissionLauncher.launch(intent);
	}

	private void requestAudioPermission()
	{
		try
		{
			Log.d("Info", "[lemonchat] PermissionChecker requestAudioPermission");

			CharSequence text = "Please grant permissions to record audio ";
			int duration = Toast.LENGTH_LONG;
			Toast toast = Toast.makeText(this.context, text, duration);
			toast.show();

			//Give user option to still opt-in the permissions
			ActivityCompat.requestPermissions(this.context, new String[] { Manifest.permission.RECORD_AUDIO }, Permissions.PERMISSIONS_RECORD_AUDIO_REQUEST_CODE);
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}

	private void requestDrawingOverOtherAppsPermission()
	{
		try
		{
			Log.d("Info", "[lemonchat] PermissionChecker requestDrawingOverOtherAppsPermission");

			String packageName = this.context.getPackageName();
			Log.d("Info", "[lemonchat] PermissionChecker checkIfAutoStartIsEnabled package name is " + packageName);

			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this.context))
			{
				Intent intent = new Intent(android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + this.context.getPackageName()));
				requestPermissionLauncher.launch(intent);
			}
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}

}
