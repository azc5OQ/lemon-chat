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

	// a copy of the log setting, because it is checked on every single printed line.
	// the saved one lives in ChatSettings and is loaded into here when the service starts
	public static volatile boolean isFileLoggingEnabled = false;

	private final IBinder binder = new LocalBinder();

	public boolean isWebViewAttachedToHiddenWindow = false;

	private WindowManager windowManager = null;

	public NotificationManager notificationManager;

	//its recommended to create separate notification channels for notifications of different purpose
	private NotificationChannel notificationChannelAppRunningInBackground;

	// v2: android locks a channel's importance once it exists, so demoting it needs a new id
	public static final String RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID = "com.lemonchat.running_in_background_notification_v2";
	private static final String RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID_OLD = "com.lemonchat.running_in_background_notification";

	private NotificationChannel notificationChannelAcceptRefuseCall;

	public static final String CALL_NOTIFICATION_CHANNEL_ID = "com.lemonchat.call_notification";
	public static final String POKE_NOTIFICATION_CHANNEL_ID = "com.lemonchat.poke_notification";

	// messages noticed by the background node runtime while the app is closed
	public static final String MESSAGE_NOTIFICATION_CHANNEL_ID = "com.lemonchat.message_notification";
	private static final int MESSAGE_NOTIFICATION_ID = 5;
	private static final int UNREAD_BADGE_NOTIFICATION_ID = 7;

	private static final int INCOMING_CALL_NOTIFICATION_ID = 2;
	private static final int POKE_NOTIFICATION_ID = 3;
	private NotificationChannel notificationChannelPoke = null;

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

	//set when a call is DECLINED: the next MainActivity resume must not fire the usual
	//come-back-from-idle request, or declining would still pull the user out of idle into the
	//root channel - which read as "reject accepted the call anyway"
	public static boolean suppressNextIdleExit = false;

	Uri callSoundUri;

	final long[] vibrationPattern = { 1000, 1000 };

	@Nullable @Override public IBinder onBind(Intent intent)
	{
		//the webview is UI and nothing else now: node owns the connection and every protocol
		//action, so it is built when a ui actually binds and never for a headless start.
		//(it still lives in the service rather than the activity so it survives rotation and
		//the activity being recreated - the overlay it once needed is long gone)
		if (this.webView == null)
		{
			this.createWebViewInServiceContext();
		}

		return this.binder;
	}

	public WebView webView;

	JavascriptJavaBridge javascriptJavaBridge;

	NodeBridge nodeBridge = null;

	// keeps the cpu ticking so node's heartbeat fires while the screen is off
	private android.os.PowerManager.WakeLock nodeWakeLock = null;

	// keeps the WIFI RADIO awake - without it wifi power save batches incoming
	// audio into 100-400ms clumps and voice stutters (the cpu lock does not cover this)
	private android.net.wifi.WifiManager.WifiLock nodeWifiLock = null;

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

				// through node, not the webview: node holds the connection, and after a boot
				// start there is no webview to call into (this used to be an NPE waiting for
				// the first incoming call - exactly what autostart is for)
				if (this.nodeBridge != null)
				{
					this.nodeBridge.sendComeFromIdle(channelId);
				}

				// the webview keeps its own idle logic: stamp the accept there too, so its gentle
				// idle (onStop during the accept transition) waits the presence grace out
				if (this.webView != null)
				{
					this.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__mark_call_accept_presence === 'function') { JavascriptJavaBridge__mark_call_accept_presence(); }", null);
				}

				//bring app to foreground by calling startActivity from service context
				//single_top so an already visible MainActivity is reused instead of stacked;
				//when the notification's accept lands here MainActivity is already coming up
				Intent intent1 = new Intent(this, MainActivity.class);
				intent1.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
				this.startActivity(intent1);

				return START_NOT_STICKY;

			case ACTION_DECLINE_CALL:
				this.notificationManager.cancel(INCOMING_CALL_NOTIFICATION_ID);
				BackgroundService.suppressNextIdleExit = true;
				return START_NOT_STICKY;
			}
		}

		return START_STICKY;
	}

	/** the user swiped the app out of the recent apps list. the service keeps running, so we
	    have to tell node ourselves that nobody is looking any more. */
	@Override public void onTaskRemoved(Intent rootIntent)
	{
		Log.d("Info", "[lemonchat] onTaskRemoved: task swiped away, reporting the ui as gone");

		MainActivity.instance = null;

		//swiping the app away means stop everything audible: FORCED idle, from any channel
		//(home/backgrounding stays gentle so in-channel music keeps playing there)
		if (this.webView != null)
		{
			this.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_go_to_idle_mode_request === 'function') { JavascriptJavaBridge__send_go_to_idle_mode_request(true); }", null);
		}

		//the app is gone, so a file picker it opened will never answer
		this.abandonPendingFileChooser();

		if (this.nodeBridge != null)
		{
			this.nodeBridge.sendUiVisible(false);
		}

		super.onTaskRemoved(rootIntent);
	}

	/** launcher-icon badge: a silent notification carrying the count is how android exposes it.
	    zero cancels it. launchers that do not support badges simply ignore the number. */
	public void showUnreadBadge(int unreadCount)
	{
		// the count on the launcher icon itself (alias switch), next to the notification badge
		LauncherIconBadge.apply(this, unreadCount);

		try
		{
			if (this.notificationManager == null)
			{
				return;
			}

			if (unreadCount <= 0)
			{
				this.notificationManager.cancel(UNREAD_BADGE_NOTIFICATION_ID);
				return;
			}

			Intent openAppIntent = new Intent(this, MainActivity.class);
			openAppIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
			PendingIntent openAppPendingIntent = PendingIntent.getActivity(this, 7, openAppIntent,
				PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			Notification badge = new NotificationCompat.Builder(this, MESSAGE_NOTIFICATION_CHANNEL_ID)
				.setContentTitle("lemon chat")
				.setContentText(unreadCount + (unreadCount == 1 ? " unread message" : " unread messages"))
				.setSmallIcon(android.R.drawable.ic_dialog_email)
				.setContentIntent(openAppPendingIntent)
				.setNumber(unreadCount)
				.setBadgeIconType(NotificationCompat.BADGE_ICON_SMALL)
				.setOnlyAlertOnce(true)
				.setSilent(true)
				.setAutoCancel(true)
				.build();

			this.notificationManager.notify(UNREAD_BADGE_NOTIFICATION_ID, badge);
		}
		catch (Exception badgeFailed)
		{
			Log.w("lemonchat", "unread badge failed", badgeFailed);
		}
	}

	/** does the device have ANY usable network right now (wifi, mobile data, ethernet) */
	public boolean isNetworkAvailable()
	{
		try
		{
			android.net.ConnectivityManager connectivity =
				(android.net.ConnectivityManager)getSystemService(Context.CONNECTIVITY_SERVICE);

			return connectivity != null && connectivity.getActiveNetwork() != null;
		}
		catch (Exception cannotTell)
		{
			return true; // unknown: never claim the network is down
		}
	}

	public void attachWebViewToInvisibleWindow()
	{
		// THE OVERLAY IS DISABLED. it existed only to keep the webview's javascript timers alive while
		// the app was in the background, and it is what forced SYSTEM_ALERT_WINDOW and the permanent
		// "displaying over other apps" notice. the background connection belongs to the embedded node
		// runtime now, which needs no window.
		//
		// the webview is simply left detached while backgrounded. android may throttle or suspend its
		// timers, and that is FINE as long as node owns the socket - but it is exactly the thing to
		// watch for if messages stop arriving while the app is closed.
		//
		// everything below is kept, unreachable, so restoring the old behaviour is deleting this return.
		if (true)
		{
			this.isWebViewAttachedToHiddenWindow = false;
			return;
		}

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

	/** forgets a file picker that is never going to come back with an answer. while one is
	    remembered the app refuses to go idle, so a forgotten one blocks idle forever. */
	public void abandonPendingFileChooser()
	{
		if (this.fileChooserCallback == null)
		{
			return;
		}

		Log.d("Info", "[lemonchat] releasing an unanswered file chooser callback");

		this.fileChooserCallback.onReceiveValue(null);
		this.fileChooserCallback = null;
	}

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

			//anything the page prints gets handed to node, which writes it to the log file.
			//without this the webview's half of a problem is not saved anywhere
			@Override public boolean onConsoleMessage(android.webkit.ConsoleMessage message)
			{
				if (BackgroundService.isFileLoggingEnabled && BackgroundService.this.nodeBridge != null)
				{
					BackgroundService.this.nodeBridge.sendLogLine(message.message()
						+ "  (" + message.sourceId() + ":" + message.lineNumber() + ")");
				}

				return true;
			}

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
		// android kills a foreground service that shows no notification within 5 seconds, and a
		// just-booted phone is slow - so the notification comes first, node startup after
		this.announceForegroundService();

		// the second autostart path, independent of the boot broadcast (see scheduleRestartJob)
		this.scheduleRestartJob();

		if (BackgroundService.isRunning == false)
		{
			BackgroundService.isRunning = true;

			// no webview here at all: starting the service means "run the client", which is
			// node's job. a ui gets one when it binds (onBind), so a boot start stays headless

			// ChatSettings only ever got its preferences from MainActivity, so a boot start left
			// it empty: sendSettings() found no json, logged "no settings to hand to node yet",
			// and node sat connected to nothing until the user opened the app
			ChatSettings.attachContext(this);

			//read the saved log setting now, before anything has a chance to print
			BackgroundService.isFileLoggingEnabled = ChatSettings.getInstance().isFileLoggingEnabled();

			// without this the cpu naps, node's heartbeat timers stop, and the server times the
			// socket out. the old overlay window kept the webview's timers alive by accident;
			// this is the honest version of the same thing
			android.os.PowerManager powerManager = (android.os.PowerManager)this.getSystemService(Context.POWER_SERVICE);
			this.nodeWakeLock = powerManager.newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "lemonchat:node");
			this.nodeWakeLock.acquire();

			// low latency exists since android 10 and needs foreground + screen on to bite;
			// high perf is the strongest mode older versions have
			try
			{
				android.net.wifi.WifiManager wifiManager = (android.net.wifi.WifiManager)this.getApplicationContext().getSystemService(Context.WIFI_SERVICE);

				if (wifiManager != null)
				{
					int wifiLockMode = (Build.VERSION.SDK_INT >= 29)
						? android.net.wifi.WifiManager.WIFI_MODE_FULL_LOW_LATENCY
						: android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF;

					this.nodeWifiLock = wifiManager.createWifiLock(wifiLockMode, "lemonchat:wifi");
					this.nodeWifiLock.acquire();
				}
			}
			catch (Exception wifiLockFailed)
			{
				Log.w("lemonchat", "wifi lock not acquired", wifiLockFailed);
			}

			// the bridge first (node needs its port and token), then node itself. node owns the
			// server connection permanently; the webview renders via the loopback
			this.nodeBridge = new NodeBridge(this);

			if (this.nodeBridge.start())
			{
				NodeRuntime.start(this.getApplicationContext(), this.nodeBridge.getPort(), this.nodeBridge.getToken());
			}

			// tell node whether the device has ANY network, now and whenever it changes. without
			// this a wifi-off phone reported "the server probably rejected the keys"
			try
			{
				android.net.ConnectivityManager connectivity =
					(android.net.ConnectivityManager)getSystemService(Context.CONNECTIVITY_SERVICE);

				if (connectivity != null)
				{
					final BackgroundService networkOwner = this;

					connectivity.registerDefaultNetworkCallback(new android.net.ConnectivityManager.NetworkCallback()
					{
						public void onAvailable(android.net.Network network)
						{
							if (networkOwner.nodeBridge != null) { networkOwner.nodeBridge.sendNetworkState(true); }
						}

						public void onLost(android.net.Network network)
						{
							if (networkOwner.nodeBridge != null) { networkOwner.nodeBridge.sendNetworkState(false); }
						}
					});

					// the callbacks only fire on CHANGE; the current answer is stated again the
					// moment node attaches to the bridge (this early one is usually dropped)
					this.nodeBridge.sendNetworkState(connectivity.getActiveNetwork() != null);
				}
			}
			catch (Exception networkWatchFailed)
			{
				Log.w("lemonchat", "network state watch unavailable", networkWatchFailed);
			}

			// every settings save, from any path, reaches BOTH sides - per-path pushes kept missing one
			final BackgroundService service = this;
			ChatSettings.onSettingsSaved = new Runnable()
			{
				public void run()
				{
					if (service.nodeBridge != null)
					{
						service.nodeBridge.sendSettings();
					}

					if (service.javascriptJavaBridge != null)
					{
						service.javascriptJavaBridge.JavaExportRequestCurrentSettingsFromAndroid();
					}
				}
			};
		}
		super.onCreate();
	}

	// the ongoing notification, every channel, then startForeground - moved out of onCreate's
	// tail so the 5 second foreground deadline is met before any slow node startup
	private void announceForegroundService()
	{
		if (Build.VERSION.SDK_INT >= 26)
		{
			this.notificationManager = (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);

			// the system will not let this notification go away, but IMPORTANCE_MIN keeps it silent and
			// parks it at the bottom of the shade instead of showing an ongoing card near the top
			this.notificationManager.deleteNotificationChannel(RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID_OLD);

			this.notificationChannelAppRunningInBackground = new NotificationChannel(RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID, "lemon chat", NotificationManager.IMPORTANCE_MIN);
			this.notificationChannelAppRunningInBackground.setShowBadge(false);
			this.notificationChannelAppRunningInBackground.setSound(null, null);
			this.notificationChannelAppRunningInBackground.enableVibration(false);
			this.notificationManager.createNotificationChannel(this.notificationChannelAppRunningInBackground);

			Notification notification = new NotificationCompat.Builder(this, RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID)
				.setContentTitle("lemon chat")
				.setContentText("running in background")
				.setSilent(true)
				.setPriority(NotificationCompat.PRIORITY_MIN)
				.setOngoing(true)
				.build();

			//sound will be default selected android system ringtone
			this.callSoundUri = RingtoneManager.getActualDefaultRingtoneUri(this, RingtoneManager.TYPE_RINGTONE);

			this.notificationChannelAcceptRefuseCall = new NotificationChannel(CALL_NOTIFICATION_CHANNEL_ID, "call", NotificationManager.IMPORTANCE_HIGH);
			this.notificationChannelAcceptRefuseCall.setVibrationPattern(this.vibrationPattern);
			this.notificationChannelAcceptRefuseCall.enableVibration(true);
			this.notificationChannelAcceptRefuseCall.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);

			this.notificationChannelAcceptRefuseCall.setSound(this.callSoundUri, new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE).build());

			this.notificationManager.createNotificationChannel(this.notificationChannelAcceptRefuseCall);

			// pokes: IMPORTANCE_HIGH is what makes android show it as a banner over whatever is on
			// screen (the way a messenger message arrives) instead of only in the shade
			this.notificationChannelPoke = new NotificationChannel(POKE_NOTIFICATION_CHANNEL_ID, "pokes", NotificationManager.IMPORTANCE_HIGH);
			this.notificationChannelPoke.enableVibration(true);
			this.notificationChannelPoke.setLockscreenVisibility(Notification.VISIBILITY_PRIVATE);
			this.notificationManager.createNotificationChannel(this.notificationChannelPoke);

			// messages the background node runtime notices while the app is closed
			NotificationChannel messageChannel = new NotificationChannel(MESSAGE_NOTIFICATION_CHANNEL_ID, "messages", NotificationManager.IMPORTANCE_HIGH);
			messageChannel.enableVibration(true);
			messageChannel.setLockscreenVisibility(Notification.VISIBILITY_PRIVATE);
			this.notificationManager.createNotificationChannel(messageChannel);

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
	}

	// job id of the restart job, any fixed number unique inside this app
	private static final int RESTART_JOB_ID = 1001;

	// registers a persisted periodic job: the system remembers it across reboots and runs it a
	// few minutes after boot even when the boot broadcast never reaches this app (huawei)
	private void scheduleRestartJob()
	{
		try
		{
			android.app.job.JobScheduler jobScheduler = (android.app.job.JobScheduler)this.getSystemService(Context.JOB_SCHEDULER_SERVICE);

			if (jobScheduler == null || jobScheduler.getPendingJob(RESTART_JOB_ID) != null)
			{
				return;
			}

			android.app.job.JobInfo restartJob = new android.app.job.JobInfo.Builder(RESTART_JOB_ID,
				new android.content.ComponentName(this, RestartJobService.class))
				.setPeriodic(15 * 60 * 1000)
				.setPersisted(true)
				.build();

			jobScheduler.schedule(restartJob);
			Log.i("lemonchat", "restart job registered");
		}
		catch (Exception scheduleFailed)
		{
			Log.w("lemonchat", "restart job registration failed", scheduleFailed);
		}
	}

	// which call we last rang for, and when. node and the webview both hear about the same call,
	// so without this the phone would ring twice for one caller
	private String lastIncomingCallKey = "";
	private long lastIncomingCallTimestamp = 0;

	public void showIncomingCall(String callerName, int channelid)
	{
		try
		{
			//ring once per call. a second one would restart the ringtone and pop the call
			//screen open again on top of the one already showing
			String callKey = callerName + "@" + channelid;
			long now = android.os.SystemClock.elapsedRealtime();

			if (callKey.equals(this.lastIncomingCallKey) && (now - this.lastIncomingCallTimestamp) < 5000)
			{
				Log.d("Info", "[lemonchat] duplicate incoming call ignored: " + callKey);
				return;
			}

			this.lastIncomingCallKey = callKey;
			this.lastIncomingCallTimestamp = now;

			//these intents get send to onStartCommand
			//basically app sends intents to itself

			// 3️⃣ Accept action PendingIntent → launches MainActivity, which forwards to the service.
			// android 12+ blocks a service tapped from a notification from starting an activity
			// (trampoline ban), so accepting used to run the js but never bring the app forward
			Intent acceptIntent = new Intent(this, MainActivity.class);
			acceptIntent.setAction(ACTION_ACCEPT_CALL);
			acceptIntent.putExtra("channelId", channelid);
			acceptIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
			PendingIntent acceptPendingIntent = PendingIntent.getActivity(this, 1, acceptIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			// 4️⃣ Decline action PendingIntent → handled by service
			Intent declineIntent = new Intent(this, BackgroundService.class);
			declineIntent.setAction(ACTION_DECLINE_CALL);
			PendingIntent declinePendingIntent = PendingIntent.getService(this, 2, declineIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			// 5️⃣ Full-screen intent → can be MainActivity (no custom CallActivity required)
			Intent fullScreenIntentActivity = new Intent(this, IncomingCallActivity.class);
			fullScreenIntentActivity.putExtra("channelId", channelid);
			fullScreenIntentActivity.putExtra("callerName", callerName);
			// NEW_TASK only: CLEAR_TOP pulled this into the chat's task, so declining revealed
			// the chat instead of returning the user to where they were
			fullScreenIntentActivity.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
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

	/**
	 * shows a poke as a normal android notification - the banner a messenger message arrives with.
	 * the web client draws pokes inside the page, which nobody sees while the app sits in the
	 * background (exactly when a poke is worth sending), so the java side has to surface it.
	 * tapping it brings the app back up.
	 */
	// raised by the node bridge for messages that arrive while the app is closed
	public void showMessageNotification(String senderName, String messageText)
	{
		try
		{
			if (this.notificationManager == null)
			{
				return;
			}

			Intent openAppIntent = new Intent(this, MainActivity.class);
			openAppIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
			PendingIntent openAppPendingIntent = PendingIntent.getActivity(this, 5, openAppIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			Notification notification = new NotificationCompat.Builder(this, MESSAGE_NOTIFICATION_CHANNEL_ID)
							    .setContentTitle(senderName)
							    .setContentText(messageText)
							    .setSmallIcon(android.R.drawable.ic_dialog_email)
							    .setContentIntent(openAppPendingIntent)
							    .setAutoCancel(true)
							    .setPriority(NotificationCompat.PRIORITY_HIGH)
							    .setCategory(Notification.CATEGORY_MESSAGE)
							    .setDefaults(Notification.DEFAULT_ALL)
							    .build();

			this.notificationManager.notify(MESSAGE_NOTIFICATION_ID, notification);
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}

	public void showPokeNotification(String senderName, String pokeMessage)
	{
		try
		{
			if (this.notificationManager == null)
			{
				return;
			}

			Intent openAppIntent = new Intent(this, MainActivity.class);
			openAppIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
			PendingIntent openAppPendingIntent = PendingIntent.getActivity(this, 3, openAppIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			Notification notification = new NotificationCompat.Builder(this, POKE_NOTIFICATION_CHANNEL_ID)
							    .setContentTitle(senderName)
							    .setContentText(pokeMessage)
							    .setStyle(new NotificationCompat.BigTextStyle().bigText(pokeMessage))
							    .setSmallIcon(android.R.drawable.ic_dialog_email) //a notification without a small icon is never shown
							    .setContentIntent(openAppPendingIntent)
							    .setAutoCancel(true)
							    .setPriority(NotificationCompat.PRIORITY_HIGH) //pre-26 devices: this is what makes it a banner
							    .setCategory(Notification.CATEGORY_MESSAGE)
							    .setDefaults(Notification.DEFAULT_ALL)
							    .build();

			this.notificationManager.notify(POKE_NOTIFICATION_ID, notification);
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}
}