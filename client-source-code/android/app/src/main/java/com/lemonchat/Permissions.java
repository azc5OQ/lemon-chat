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

		//check notification permission (asked at most once; a denial must not block the checks below)
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
		{
			if (ContextCompat.checkSelfPermission(this.context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED && this.notificationPermissionRequestedOnce == false)
			{
				this.notificationPermissionRequestedOnce = true;
				this.requestNotificationPermission();
				return;
			}
		}

		//check draw over other apps permisison
		if (Settings.canDrawOverlays(this.context) == false && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
		{
			CharSequence text = "You must enable drawing over other apps! ";
			int duration = Toast.LENGTH_LONG;
			Toast toast = Toast.makeText(this.context, text, duration);
			toast.show();

			this.requestDrawingOverOtherAppsPermission(); //so app can work even when in background
			return;
		}

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
		this.requestPermissionLauncher1.launch(Manifest.permission.POST_NOTIFICATIONS);
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
