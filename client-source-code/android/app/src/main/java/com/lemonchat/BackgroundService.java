package com.lemonchat;

import static android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
import static android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_PHONE_CALL;
import static androidx.core.app.ActivityCompat.startActivityForResult;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.PixelFormat;
import android.media.AudioAttributes;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowManager;
import android.webkit.PermissionRequest;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import androidx.annotation.Nullable;
import androidx.core.app.ActivityCompat;
import androidx.core.app.NotificationCompat;
import androidx.core.content.ContextCompat;

public class BackgroundService extends Service
{
	public class LocalBinder extends Binder
	{
		BackgroundService getService()
		{
			return BackgroundService.this;
		}
	}

	public static volatile boolean isRunning = false;

	private final IBinder binder = new LocalBinder();

	public boolean isWebViewAttachedToHiddenWindow = false;

	private WindowManager windowManager = null;

	public NotificationManager notificationManager;

	//its recommended to create separate notification channels for notifications of different purpose
	private NotificationChannel notificationChannelAppRunningInBackground;

	public static final String RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID = "com.lemonchat.running_in_background_notification";

	private NotificationChannel notificationChannelAcceptRefuseCall;

	public static final String CALL_NOTIFICATION_CHANNEL_ID = "com.lemonchat.call_notification";

	private static final int INCOMING_CALL_NOTIFICATION_ID = 2;

	public static final String ACTION_INCOMING_CALL = "ACTION_INCOMING_CALL";
	public static final String ACTION_ACCEPT_CALL = "ACTION_ACCEPT_CALL";
	public static final String ACTION_DECLINE_CALL = "ACTION_DECLINE_CALL";

	//java part has to keep track of client status, if he is idle or not
	//there is tricky situation
	//when app goes to background while client is still connecting to a server
	//client will remain in root channel without option to call him, because at the time app went to background
	//he could not send that info to server because of non existing onnection
	//

	public static boolean isClientInIdleMode = false;

	Uri callSoundUri;

	final long[] vibrationPattern = { 1000, 1000 };

	@Nullable @Override public IBinder onBind(Intent intent)
	{
		return this.binder;
	}

	public WebView webView;

	JavascriptJavaBridge javascriptJavaBridge;

	///this function is not for initialization, thats what onCreate is for
	///every time intent is passed to this service, onStartCommand gets run
	///for example the ACTION_ACCEPT_CALL, basically app passes intent to itself
	@Override public int onStartCommand(Intent intent, int flags, int startId)
	{
		if (intent != null && intent.getAction() != null)
		{
			switch (intent.getAction())
			{
			case ACTION_ACCEPT_CALL:
				this.notificationManager.cancel(INCOMING_CALL_NOTIFICATION_ID);

				int channelId = intent.getIntExtra("channelId", 0);
				this.webView.evaluateJavascript("JavascriptJavaBridge__send_come_from_idle_mode_request(" + channelId + ");", null);

				//bring app to foreground by calling startActivity from service context
				Intent intent1 = new Intent(this, MainActivity.class);
				intent1.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); //needed flag
				this.startActivity(intent1);

				return START_NOT_STICKY;

			case ACTION_DECLINE_CALL:
				this.notificationManager.cancel(INCOMING_CALL_NOTIFICATION_ID);
				return START_NOT_STICKY;
			}
		}

		return START_STICKY;
	}

	public void attachWebViewToInvisibleWindow()
	{
		try
		{
			if (this.windowManager == null)
			{
				this.windowManager = (WindowManager)this.getSystemService(WINDOW_SERVICE);
			}

			if (this.windowManager != null)
			{
				WindowManager.LayoutParams params = null;

				if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
				{
					params = new WindowManager.LayoutParams(0, 0,
										WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, //or is it TYPE_APPLICATION_PANEL
										WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, PixelFormat.TRANSLUCENT);
				}
				else
				{
					params = new WindowManager.LayoutParams(0, 0, WindowManager.LayoutParams.TYPE_PHONE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, PixelFormat.TRANSLUCENT);
				}

				params.gravity = Gravity.TOP | Gravity.START;
				params.x = 0;
				params.y = 0;
				params.width = 0;
				params.height = 0;

				try
				{
					windowManager.addView(this.webView, params);
					this.isWebViewAttachedToHiddenWindow = true;
				}
				catch (Exception e)
				{
					e.printStackTrace();
				}
			}
			else
			{
				Log.d("Info", "[lemonchat] this.windowManager != null ");
			}
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}

	public void detachWebViewFromInvisibleWindow()
	{
		try
		{
			this.windowManager = (WindowManager)this.getSystemService(WINDOW_SERVICE);

			if (this.webView != null && this.windowManager != null)
			{
				try
				{
					this.windowManager.removeViewImmediate(this.webView);
					this.isWebViewAttachedToHiddenWindow = false;
				}
				catch (Exception e)
				{
					e.printStackTrace();
				}
			}
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}

	public void askForPermission(String origin, String permission, int requestCode, PermissionRequest myRequest)
	{
		Log.d("WebView", "inside askForPermission for" + origin + "with" + permission);

		if (MainActivity.instance == null)
		{
			return;
		}

		if (ContextCompat.checkSelfPermission(getApplicationContext(), permission) != PackageManager.PERMISSION_GRANTED)
		{
			// Should we show an explanation?
			if (ActivityCompat.shouldShowRequestPermissionRationale(MainActivity.instance, permission))
			{
				// Show an expanation to the user *asynchronously* -- don't block
				// this thread waiting for the user's response! After the user
				// sees the explanation, try again to request the permission.
			}
			else
			{
				// No explanation needed, we can request the permission.

				ActivityCompat.requestPermissions(MainActivity.instance, new String[] { permission }, requestCode);
			}
		}
		else
		{
			myRequest.grant(myRequest.getResources());
		}
	}

	public ValueCallback<Uri[]> fileChooserCallback;

	public void createWebViewInServiceContext()
	{
		this.javascriptJavaBridge = new JavascriptJavaBridge(this, ChatSettings.getInstance(), this);

		//create WebView in service context
		this.webView = new WebView(this);
		this.webView.setWebViewClient(new WebViewClient());
		this.webView.setWebChromeClient(new WebChromeClient() {
			@Override public void onPermissionRequest(final PermissionRequest request)
			{
				for (String permission : request.getResources())
				{
					switch (permission)
					{
					case "android.webkit.resource.AUDIO_CAPTURE": {
						Log.d("Info", "[lemonchat] android.webkit.resource.AUDIO_CAPTURE :");
						int MY_PERMISSIONS_REQUEST_RECORD_AUDIO = 101;
						askForPermission(request.getOrigin().toString(), Manifest.permission.RECORD_AUDIO, MY_PERMISSIONS_REQUEST_RECORD_AUDIO, request);
						break;
					}
					}
				}
			}

			//			@Override
			//			public void onPermissionRequest(final PermissionRequest request) {
			//				request.grant(request.getResources());
			//			}

			//so WebView allows user to select file form disk..
			@Override public boolean onShowFileChooser(WebView vw, ValueCallback<Uri[]> filePathCallback, FileChooserParams fileChooserParams)
			{
				if (fileChooserCallback != null)
				{
					fileChooserCallback.onReceiveValue(null);
				}
				fileChooserCallback = filePathCallback;

				Intent selectionIntent = new Intent(Intent.ACTION_GET_CONTENT);
				selectionIntent.addCategory(Intent.CATEGORY_OPENABLE);
				selectionIntent.setType("*/*");
				selectionIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

				Intent chooserIntent = new Intent(Intent.ACTION_CHOOSER);
				chooserIntent.putExtra(Intent.EXTRA_INTENT, selectionIntent);
				startActivityForResult(MainActivity.instance, chooserIntent, MainActivity.FILE_CHOOSER_RESULT_CODE, null);

				return true;
			}
		});

		this.webView.addJavascriptInterface(javascriptJavaBridge, "Android");
		//hmmm
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P)
		{
			this.webView.setRendererPriorityPolicy(android.webkit.WebView.RENDERER_PRIORITY_IMPORTANT, false);
		}

		WebSettings webSettings = this.webView.getSettings();
		webSettings.setJavaScriptEnabled(true);
		webSettings.setDomStorageEnabled(true);
		webSettings.setMediaPlaybackRequiresUserGesture(false);
		webSettings.setSupportZoom(false);
		webSettings.setBuiltInZoomControls(false);
		webSettings.setAllowContentAccess(true);
		webSettings.setDomStorageEnabled(true); //required for some js?
		webSettings.setAllowFileAccess(true);
		webSettings.setLoadWithOverviewMode(false); //zooms page out
		//webSettings.setUseWideViewPort(true); //sets device screen width to virtual size of 960pixels
		webSettings.setSaveFormData(false);
		webSettings.setJavaScriptCanOpenWindowsAutomatically(true);
		webSettings.setSupportMultipleWindows(false);
		webSettings.setCacheMode(WebSettings.LOAD_NO_CACHE);

		//note for anyone who might want to edit client.html in asset directory, dont, it gets overwritten by gradle script on build time
		//gradle overwrites client.html asset with client.html from root client directory on each build
		this.webView.loadUrl("file:///android_asset/client.html"); //path to android_asset is android\app\src\main\assets

		Log.d("Info", "[lemonchat] WebView loaded :");
	}

	@Override public void onCreate()
	{
		if (BackgroundService.isRunning == false)
		{
			BackgroundService.isRunning = true;

			new Thread(new Runnable() {
				@Override public void run()
				{
					int number = 1;
					while (true)
					{
						Log.d("Info", "[lemonchat] hello from service for :" + number + "th time");
						number++;

						try
						{
							Thread.sleep(15000);
						}
						catch (InterruptedException ex)
						{
							Log.d("Info", "[lemonchat] Service Exception :" + ex.getMessage());
						}
					}
				}
			}).start();

			this.createWebViewInServiceContext();
		}
		if (Build.VERSION.SDK_INT >= 26)
		{
			this.notificationManager = (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);

			this.notificationChannelAppRunningInBackground = new NotificationChannel(RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID, "lemon chat", NotificationManager.IMPORTANCE_DEFAULT);
			this.notificationManager.createNotificationChannel(this.notificationChannelAppRunningInBackground);

			Notification notification = new NotificationCompat.Builder(this, RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID).setContentTitle("lemon chat").setSilent(true).setContentText("running in background").build();

			//sound will be default selected android system ringtone
			this.callSoundUri = RingtoneManager.getActualDefaultRingtoneUri(this, RingtoneManager.TYPE_RINGTONE);

			this.notificationChannelAcceptRefuseCall = new NotificationChannel(CALL_NOTIFICATION_CHANNEL_ID, "call", NotificationManager.IMPORTANCE_HIGH);
			this.notificationChannelAcceptRefuseCall.setVibrationPattern(this.vibrationPattern);
			this.notificationChannelAcceptRefuseCall.enableVibration(true);
			this.notificationChannelAcceptRefuseCall.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);

			this.notificationChannelAcceptRefuseCall.setSound(this.callSoundUri, new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE).build());

			this.notificationManager.createNotificationChannel(this.notificationChannelAcceptRefuseCall);

			int notificationId = 1;

			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
			{
				this.startForeground(notificationId, notification, FOREGROUND_SERVICE_TYPE_MICROPHONE | FOREGROUND_SERVICE_TYPE_PHONE_CALL);
			}
			else
			{
				this.startForeground(notificationId, notification);
			}
		}

		super.onCreate();
	}

	public void showIncomingCall(String callerName, int channelid)
	{
		try
		{
			//these intents get send to onStartCommand
			//basically app sends intents to itself

			// 3️⃣ Accept action PendingIntent → handled by service
			Intent acceptIntent = new Intent(this, BackgroundService.class);
			acceptIntent.setAction(ACTION_ACCEPT_CALL);
			acceptIntent.putExtra("channelId", channelid);
			PendingIntent acceptPendingIntent = PendingIntent.getService(this, 1, acceptIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			// 4️⃣ Decline action PendingIntent → handled by service
			Intent declineIntent = new Intent(this, BackgroundService.class);
			declineIntent.setAction(ACTION_DECLINE_CALL);
			PendingIntent declinePendingIntent = PendingIntent.getService(this, 2, declineIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			// 5️⃣ Full-screen intent → can be MainActivity (no custom CallActivity required)
			Intent fullScreenIntentActivity = new Intent(this, IncomingCallActivity.class);
			fullScreenIntentActivity.putExtra("channelId", channelid);
			fullScreenIntentActivity.putExtra("callerName", callerName);
			fullScreenIntentActivity.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
			PendingIntent fullScreenIntent = PendingIntent.getActivity(this, 0, fullScreenIntentActivity, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			androidx.core.app.Person caller = new androidx.core.app.Person.Builder().setName(callerName).setImportant(true).build();

			Notification notification = new NotificationCompat.Builder(this, CALL_NOTIFICATION_CHANNEL_ID)
							    .setContentIntent(fullScreenIntent)
							    .setFullScreenIntent(fullScreenIntent, true)
							    .setContentText(callerName)
							    .setContentTitle("Incoming call")
							    .setAutoCancel(true)
							    .setOngoing(true) //cant be cancelled by swipe, only be accept / refuse
							    .setTimeoutAfter(15000)
							    .setPriority(NotificationCompat.PRIORITY_HIGH)
							    .setCategory(Notification.CATEGORY_ALARM)
							    .setSound(this.callSoundUri)
							    .setSmallIcon(android.R.drawable.ic_menu_call) //wont work without this
							    .setVibrate(this.vibrationPattern)
							    .setStyle(NotificationCompat.CallStyle.forIncomingCall(caller, declinePendingIntent, acceptPendingIntent))
							    .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
							    .addPerson(caller)
							    .build();

			notification.flags |= NotificationCompat.FLAG_INSISTENT; //notification should persistent in the status bar until its dismissed

			this.notificationManager.notify(INCOMING_CALL_NOTIFICATION_ID, notification);
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}
}