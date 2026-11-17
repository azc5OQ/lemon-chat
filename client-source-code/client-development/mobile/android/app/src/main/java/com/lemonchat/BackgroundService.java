package com.lemonchat;

import static androidx.core.app.ActivityCompat.startActivityForResult;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.MutableContextWrapper;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
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
import android.webkit.MimeTypeMap;
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
		/**
		 * Returns the service instance associated with this local binding.
		 *
		 * @return the bound background service
		 */
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
	private Notification foregroundNotification;
	private boolean foregroundAdmitted;
	private long microphoneGeneration = -1;

	//its recommended to create separate notification channels for notifications of different purpose
	private NotificationChannel notificationChannelAppRunningInBackground;

	// android retains the importance of an existing notification channel
	public static final String RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID = "com.lemonchat.running_in_background_notification_v3";
	private static final String RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID_OLD = "com.lemonchat.running_in_background_notification_v2";

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

	/**
	 * Creates the UI WebView only when an activity binds to the background service.
	 *
	 * @param intent binding request from the activity
	 * @return the local service binder
	 */
	@Nullable @Override public IBinder onBind(Intent intent)
	{
		if (!this.foregroundAdmitted)
		{
			return null;
		}
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

	public volatile WebView webView;

	// the context of the WebView: the activity that shows it, or this service while no activity shows it.
	// the list of a <select> and other pop-up windows of the page need the window of an activity
	private MutableContextWrapper webViewContext;

	JavascriptJavaBridge javascriptJavaBridge;

	volatile NodeBridge nodeBridge = null;

	// keeps the cpu ticking so node's heartbeat fires while the screen is off
	private android.os.PowerManager.WakeLock nodeWakeLock = null;

	// keeps the WIFI RADIO awake - without it wifi power save batches incoming
	// audio into 100-400ms clumps and voice stutters (the cpu lock does not cover this)
	private android.net.wifi.WifiManager.WifiLock nodeWifiLock = null;

	/**
	 * Handles a native call action while keeping the service eligible for sticky restart.
	 *
	 * @param intent service command, or {@code null} after a sticky restart
	 * @param flags framework start flags
	 * @param startId identifier of this start request
	 * @return {@link #START_STICKY}
	 */
	@Override public int onStartCommand(Intent intent, int flags, int startId)
	{
		if (!this.foregroundAdmitted)
		{
			return START_NOT_STICKY;
		}
		this.handleCallAction(intent);
		return START_STICKY;
	}

	/**
	 * Consumes only the current unexpired native call capability and forwards its action to Node.
	 *
	 * @param intent Accept or Decline intent carrying the call and session IDs
	 */
	public synchronized void handleCallAction(Intent intent)
	{
		if (intent != null && (ACTION_ACCEPT_CALL.equals(intent.getAction()) || ACTION_DECLINE_CALL.equals(intent.getAction())))
		{
			String callId = intent.getStringExtra("call_id");
			String sessionId = intent.getStringExtra("session_id");
			if (!this.isCurrentCall(callId, sessionId))
			{
				return;
			}
			boolean accepted = ACTION_ACCEPT_CALL.equals(intent.getAction());
			this.activeCallId = "";
			this.notificationManager.cancel(INCOMING_CALL_NOTIFICATION_ID);
			if (this.nodeBridge != null)
			{
				this.nodeBridge.sendCallAction(accepted ? "accept" : "decline", callId, sessionId);
			}
			BackgroundService.suppressNextIdleExit = !accepted;
			if (accepted && this.webView != null && this.isTrustedPage())
			{
				this.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__mark_call_accept_presence === 'function')\n{\n    JavascriptJavaBridge__mark_call_accept_presence();\n}", null);
			}
		}
	}

	/**
	 * Stops page media and forces Node into idle when the user removes the task.
	 * Native presence cleanup runs even if the page is absent or its callback fails.
	 *
	 * @param rootIntent original task intent supplied by Android
	 */
	@Override public void onTaskRemoved(Intent rootIntent)
	{
		MainActivity.instance = null;
		this.retirePageMedia();
		try
		{
			// The page stops capture/playback; Node receives the presence command independently.
			try
			{
				if (this.webView != null && this.isTrustedPage())
				{
					this.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_go_to_idle_mode_request === 'function')\n{\n    JavascriptJavaBridge__send_go_to_idle_mode_request(true);\n}", null);
				}
			}
			catch (RuntimeException unavailable)
			{
				Log.w("lemonchat", "task removal media cleanup unavailable");
			}
			try
			{
				this.abandonPendingFileChooser();
			}
			catch (RuntimeException unavailable)
			{
				Log.w("lemonchat", "task removal chooser cleanup unavailable");
			}
			if (this.nodeBridge != null)
			{
				this.nodeBridge.sendUiVisible(false, false, true);
			}
		}
		finally
		{
			super.onTaskRemoved(rootIntent);
		}
	}

	/**
	 * Updates the launcher badge and its silent supporting notification.
	 *
	 * @param unreadCount unread total, or zero to remove the notification
	 */
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

	/**
	 * Checks whether Android currently reports an active network.
	 *
	 * @return false if Android confirms there is no active network, otherwise true
	 */
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

	/**
	 * Leaves the WebView detached while Node maintains the background connection.
	 * The legacy overlay implementation remains disabled.
	 */
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

	/**
	 * @brief Sets the context of the WebView. While an activity shows the WebView, the activity is its
	 *        context, so that pop-up windows of the page (the list of a <select>) belong to the activity's
	 *        window. While no activity shows it, this service is its context, so that no closed activity
	 *        is kept in memory.
	 *
	 * @param Context owner -> the activity that shows the WebView, or null for this service
	 *
	 * @return void
	 */
	public void setWebViewWindowOwner(Context owner)
	{
		// replace the context
		if (this.webViewContext != null)
		{
			this.webViewContext.setBaseContext(owner != null ? owner : this);
		}
	}

	/**
	 * Attempts to remove a legacy overlay attachment before reusing or destroying the WebView.
	 */
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

	private PermissionRequest pendingPermission;
	private MainActivity permissionOwner;
	private boolean permissionPromptPending;
	private long permissionGeneration;
	private volatile long pageGeneration;
	private volatile boolean trustedPage;
	private MainActivity fileChooserOwner;
	private long fileChooserGeneration;
	private int fileChooserRequestCode;
	private static int nextFileChooserRequestCode = 20000;
	private android.net.ConnectivityManager.NetworkCallback networkCallback;

	/**
	 * Reports whether the current WebView document passed the bundled-page check.
	 *
	 * @return whether native callbacks may target the current document
	 */
	public boolean isTrustedPage()
	{
		return this.trustedPage;
	}

	/**
	 * Identifies the current document generation for delayed native callbacks.
	 *
	 * @return the monotonically advancing page generation
	 */
	public long getPageGeneration()
	{
		return this.pageGeneration;
	}

	/**
	 * Revokes current document authority and settles its pending picker and permission callbacks.
	 */
	public void invalidatePage()
	{
		this.pageGeneration++;
		this.trustedPage = false;
		this.microphoneGeneration = -1;
		this.retirePageMedia();
		this.abandonPendingFileChooser();
		if (this.pendingPermission != null)
		{
			this.pendingPermission.deny();
			this.pendingPermission = null;
		}
	}

	/**
	 * Starts a new document generation and evaluates whether its URL is trusted.
	 *
	 * @param url document URL reported by the WebView client
	 */
	public void beginPage(String url)
	{
		this.invalidatePage();
		this.trustedPage = TrustedWebViewClient.trusted(url);
		this.resumePageMedia(MainActivity.instance);
	}

	/**
	 * Resolves or requests native microphone permission for the current trusted page.
	 * Requests without a live activity or from an untrusted origin are denied.
	 *
	 * @param origin origin supplied by the WebView permission request
	 * @param permission Android permission to check or request
	 * @param requestCode activity permission request code
	 * @param request WebView audio capture request to settle
	 */
	public void askForPermission(String origin, String permission, int requestCode, PermissionRequest request)
	{
		MainActivity activity = MainActivity.instance;
		if (!this.trustedPage || activity == null || !activity.isMediaActivityVisible()
			|| !("file://".equals(origin) || "file:///".equals(origin)))
		{
			request.deny();
			return;
		}
		if (ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED)
		{
			if (this.admitMicrophone(activity))
			{
				request.grant(new String[] { PermissionRequest.RESOURCE_AUDIO_CAPTURE });
			}
			else
			{
				request.deny();
			}
			return;
		}
		if (this.permissionPromptPending)
		{
			request.deny();
			return;
		}
		this.pendingPermission = request;
		this.permissionOwner = activity;
		this.permissionPromptPending = true;
		this.permissionGeneration = this.pageGeneration;
		try
		{
			ActivityCompat.requestPermissions(activity, new String[] { permission }, requestCode);
		}
		catch (RuntimeException unavailable)
		{
			this.cancelPendingPermission(activity);
		}
	}

	/**
	 * Settles the pending audio request only for its owning activity and document generation.
	 *
	 * @param owner activity receiving the Android permission result
	 */
	public void completeAudioPermission(MainActivity owner)
	{
		if (owner != this.permissionOwner)
		{
			return;
		}
		if (MainActivity.instance != owner || !owner.isMediaActivityVisible())
		{
			return;
		}
		PermissionRequest request = this.pendingPermission;
		this.pendingPermission = null;
		this.permissionOwner = null;
		this.permissionPromptPending = false;
		if (request == null)
		{
			return;
		}
		if (this.trustedPage && this.permissionGeneration == this.pageGeneration
			&& this.admitMicrophone(owner))
		{
			request.grant(new String[] { PermissionRequest.RESOURCE_AUDIO_CAPTURE });
		}
		else
		{
			request.deny();
		}
	}

	/**
	 * Denies a pending WebView permission request and releases its native owner.
	 *
	 * @param owner activity whose request is being cancelled, or {@code null} to cancel any request
	 */
	public void cancelPendingPermission(MainActivity owner)
	{
		if (owner != null && owner != this.permissionOwner)
		{
			return;
		}
		PermissionRequest request = this.pendingPermission;
		this.pendingPermission = null;
		this.permissionOwner = null;
		this.permissionPromptPending = false;
		if (request != null)
		{
			request.deny();
		}
	}

	/**
	 * Delivers a selected file only to the matching activity, request and current trusted document.
	 *
	 * @param activity activity receiving the picker result
	 * @param requestCode picker request code
	 * @param resultCode Android result status
	 * @param data returned picker intent, which may be {@code null}
	 */
	public void completeFileChooser(MainActivity activity, int requestCode, int resultCode, Intent data)
	{
		if (this.fileChooserCallback == null || activity != this.fileChooserOwner || requestCode != this.fileChooserRequestCode)
		{
			return;
		}
		ValueCallback<Uri[]> callback = this.fileChooserCallback;
		this.fileChooserCallback = null;
		this.fileChooserOwner = null;
		Uri[] result = null;
		if (this.trustedPage && this.fileChooserGeneration == this.pageGeneration && resultCode == android.app.Activity.RESULT_OK
			&& data != null && data.getData() != null)
		{
			result = new Uri[] { data.getData() };
		}
		callback.onReceiveValue(result);
	}

	public ValueCallback<Uri[]> fileChooserCallback;

	/**
	 * Cancels the picker callback only when the specified activity owns it.
	 *
	 * @param owner activity being closed or replaced
	 */
	public void abandonPendingFileChooser(MainActivity owner)
	{
		if (owner == this.fileChooserOwner)
		{
			this.abandonPendingFileChooser();
		}
	}

	/**
	 * Settles and forgets a picker callback that can no longer return a usable file.
	 */
	public void abandonPendingFileChooser()
	{
		if (this.fileChooserCallback == null)
		{
			return;
		}

		Log.d("Info", "[lemonchat] releasing an unanswered file chooser callback");

		ValueCallback<Uri[]> callback = this.fileChooserCallback;
		this.fileChooserCallback = null;
		this.fileChooserOwner = null;
		try
		{
			callback.onReceiveValue(null);
		}
		catch (RuntimeException unavailable)
		{
			Log.w("lemonchat", "file chooser page already unavailable");
		}
	}

	/**
	 * Converts the accept list of the page's file input (".mp3", "image/*", "audio/mpeg") into the MIME types
	 * the Android file picker filters by. A file name extension is looked up in Android's MIME table; an entry
	 * Android does not know (".lmn") is left out.
	 *
	 * @param params file chooser parameters the WebView passed for the input
	 *
	 * @return MIME types, empty when the input accepts every file or no entry is known
	 */
	private static String[] acceptedMimeTypes(WebChromeClient.FileChooserParams params)
	{
		java.util.ArrayList<String> types = new java.util.ArrayList<>();
		String[] accepted = params != null ? params.getAcceptTypes() : null;
		if (accepted == null)
		{
			return new String[0];
		}
		for (String entry : accepted)
		{
			for (String part : entry.split(","))
			{
				String type = part.trim().toLowerCase(java.util.Locale.ROOT);

				// extension to type
				if (type.startsWith("."))
				{
					type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(type.substring(1));
				}
				if (type != null && type.contains("/") && !types.contains(type))
				{
					types.add(type);
				}
			}
		}
		return types.toArray(new String[0]);
	}

	/**
	 * Creates the service WebView, installs trusted native interfaces and loads the bundled page.
	 */
	public void createWebViewInServiceContext()
	{
		this.javascriptJavaBridge = new JavascriptJavaBridge(this, ChatSettings.getInstance(), this);

		//create WebView in service context
		this.webViewContext = new MutableContextWrapper(this);
		this.webView = new WebView(this.webViewContext);
		this.webView.setWebViewClient(new TrustedWebViewClient(this));
		this.webView.setWebChromeClient(new WebChromeClient() {
			@Override public void onPermissionRequest(final PermissionRequest request)
			{
				String[] resources = request.getResources();
				if (resources.length != 1 || !PermissionRequest.RESOURCE_AUDIO_CAPTURE.equals(resources[0]))
				{
					request.deny();
					return;
				}
				askForPermission(request.getOrigin().toString(), Manifest.permission.RECORD_AUDIO, 101, request);
			}

			@Override public void onPermissionRequestCanceled(PermissionRequest request)
			{
				if (BackgroundService.this.pendingPermission == request)
				{
					BackgroundService.this.pendingPermission = null;
				}
			}

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
				BackgroundService.this.abandonPendingFileChooser();
				MainActivity activity = MainActivity.instance;
				if (!BackgroundService.this.trustedPage || activity == null || activity.isFinishing() || nextFileChooserRequestCode > 65000)
				{
					filePathCallback.onReceiveValue(null);
					return true;
				}
				fileChooserCallback = filePathCallback;
				fileChooserOwner = activity;
				fileChooserGeneration = pageGeneration;
				fileChooserRequestCode = nextFileChooserRequestCode++;
				Intent selectionIntent = new Intent(Intent.ACTION_GET_CONTENT);
				selectionIntent.addCategory(Intent.CATEGORY_OPENABLE);
				selectionIntent.setType("*/*");
				selectionIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

				// accepted types only
				String[] mimeTypes = BackgroundService.acceptedMimeTypes(fileChooserParams);
				if (mimeTypes.length > 0)
				{
					selectionIntent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
				}
				try
				{
					activity.startActivityForResult(Intent.createChooser(selectionIntent, "Select file"), fileChooserRequestCode);
				}
				catch (RuntimeException unavailable)
				{
					BackgroundService.this.abandonPendingFileChooser();
				}
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
		webSettings.setJavaScriptCanOpenWindowsAutomatically(false);
		webSettings.setSupportMultipleWindows(false);
		webSettings.setCacheMode(WebSettings.LOAD_NO_CACHE);

		//note for anyone who might want to edit client.html in asset directory, dont, it gets overwritten by gradle script on build time
		//gradle overwrites client.html asset with client.html from root client directory on each build
		this.webView.loadUrl("file:///android_asset/client.html"); //path to android_asset is android\app\src\main\assets

		Log.d("Info", "[lemonchat] WebView loaded :");
	}

	/**
	 * Starts the foreground notification, initializes settings and attaches the process runtime.
	 * Network and settings listeners then publish changes to the same Node owner.
	 */
	@Override public void onCreate()
	{
		super.onCreate();
		// publish the notification before preparing the runtime
		if (!this.announceForegroundService())
		{
			this.stopSelf();
			return;
		}

		// the second autostart path, independent of the boot broadcast (see scheduleRestartJob)
		this.scheduleRestartJob();

		{
			BackgroundService.isRunning = true;

			// no webview here at all: starting the service means "run the client", which is
			// node's job. a ui gets one when it binds (onBind), so a boot start stays headless

			// ChatSettings only ever got its preferences from MainActivity, so a boot start left
			// it empty: sendSettings() found no json, logged "no settings to hand to node yet",
			// and node sat connected to nothing until the user opened the app
			ChatSettings.attachContext(this);
			ChatSettings.getInstance().loadPersistedState();

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
			this.nodeBridge = AndroidRuntimeHost.attach(this);

			// tell node whether the device has ANY network, now and whenever it changes. without
			// this a wifi-off phone reported "the server probably rejected the keys"
			try
			{
				android.net.ConnectivityManager connectivity =
					(android.net.ConnectivityManager)getSystemService(Context.CONNECTIVITY_SERVICE);

				if (connectivity != null)
				{
					final BackgroundService networkOwner = this;

					this.networkCallback = new android.net.ConnectivityManager.NetworkCallback()
					{
						public void onAvailable(android.net.Network network)
						{
							NodeBridge bridge = networkOwner.nodeBridge;
							if (bridge != null)
							{
								bridge.sendNetworkState(networkOwner.isNetworkAvailable());
							}
						}

						public void onLost(android.net.Network network)
						{
							NodeBridge bridge = networkOwner.nodeBridge;
							if (bridge != null)
							{
								bridge.sendNetworkState(networkOwner.isNetworkAvailable());
							}
						}
					};
					connectivity.registerDefaultNetworkCallback(this.networkCallback);

					// the callbacks only fire on CHANGE; the current answer is stated again the
					// moment node attaches to the bridge (this early one is usually dropped)
					if (this.nodeBridge != null)
					{
						this.nodeBridge.sendNetworkState(connectivity.getActiveNetwork() != null);
					}
				}
			}
			catch (Exception networkWatchFailed)
			{
				this.networkCallback = null;
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
	}

	/**
	 * Revokes page authority, detaches native listeners and releases service resources.
	 * The shared Node runtime is not restarted or replaced by service teardown.
	 */
	@Override public void onDestroy()
	{
		this.foregroundAdmitted = false;
		this.microphoneGeneration = -1;
		try
		{
			BackgroundService.isRunning = false;
			this.trustedPage = false;
			this.pageGeneration++;
			this.abandonPendingFileChooser();
			this.cancelPendingPermission(null);
			AndroidRuntimeHost.detach(this);
			this.nodeBridge = null;
			ChatSettings.onSettingsSaved = null;
			if (this.networkCallback != null)
			{
				android.net.ConnectivityManager connectivity = (android.net.ConnectivityManager)getSystemService(Context.CONNECTIVITY_SERVICE);
				try
				{
					connectivity.unregisterNetworkCallback(this.networkCallback);
				}
				catch (RuntimeException alreadyReleased)
				{
					Log.w("lemonchat", "network listener already released");
				}
				this.networkCallback = null;
			}
			if (this.nodeWakeLock != null && this.nodeWakeLock.isHeld())
			{
				this.nodeWakeLock.release();
			}
			if (this.nodeWifiLock != null && this.nodeWifiLock.isHeld())
			{
				this.nodeWifiLock.release();
			}
			this.detachWebViewFromInvisibleWindow();
			if (this.webView != null)
			{
				this.webView.removeJavascriptInterface("Android");
				this.webView.destroy();
				this.webView = null;
			}
		}
		finally
		{
			super.onDestroy();
		}
	}

	/**
	 * creates notification channels before runtime preparation
	 *
	 * @return whether Android admitted the messaging service
	 */
	private boolean announceForegroundService()
	{
		if (Build.VERSION.SDK_INT >= 26)
		{
			this.notificationManager = (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);

			// replace the old minimum importance channel
			this.notificationManager.deleteNotificationChannel(RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID_OLD);
			this.notificationManager.deleteNotificationChannel("com.lemonchat.running_in_background_notification");

			this.notificationChannelAppRunningInBackground = new NotificationChannel(RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID, "lemon chat", NotificationManager.IMPORTANCE_LOW);
			this.notificationChannelAppRunningInBackground.setShowBadge(false);
			this.notificationChannelAppRunningInBackground.setSound(null, null);
			this.notificationChannelAppRunningInBackground.enableVibration(false);
			this.notificationManager.createNotificationChannel(this.notificationChannelAppRunningInBackground);

			Notification notification = new NotificationCompat.Builder(this, RUNNING_IN_BACKGROUND_NOTIFICATION_CHANNEL_ID)
				.setContentTitle("lemon chat")
				.setContentText("Running in background")
				.setSmallIcon(android.R.drawable.ic_dialog_email)
				.setSilent(true)
				.setPriority(NotificationCompat.PRIORITY_LOW)
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

			this.foregroundNotification = notification;
			return this.updateForeground(false, false);
		}
		return false;
	}

	/**
	 * restores media eligibility through the visible trusted page
	 *
	 * @param owner activity presenting the page
	 */
	public void resumePageMedia(MainActivity owner)
	{
		if (owner == null || owner != MainActivity.instance || !owner.isMediaActivityVisible() || !this.trustedPage || !this.foregroundAdmitted)
		{
			return;
		}
		boolean microphone = this.microphoneGeneration == this.pageGeneration && ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
		this.updateForeground(true, microphone);
	}

	/** releases media types until the visible page returns */
	private void retirePageMedia()
	{
		if (this.foregroundAdmitted)
		{
			this.updateForeground(false, false);
		}
	}

	/**
	 * admits capture before WebView receives its microphone grant
	 *
	 * @param owner activity requesting capture
	 * @return whether the current visible document can capture
	 */
	private boolean admitMicrophone(MainActivity owner)
	{
		if (!this.foregroundAdmitted || !this.trustedPage || owner == null || owner != MainActivity.instance || !owner.isMediaActivityVisible() || ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
		{
			return false;
		}
		if (!this.updateForeground(true, true))
		{
			return false;
		}
		this.microphoneGeneration = this.pageGeneration;
		return true;
	}

	/**
	 * applies the foreground types before the corresponding work starts
	 *
	 * @param playback whether visible UI admitted playback
	 * @param microphone whether visible UI admitted microphone capture
	 * @return whether Android admitted the requested types
	 */
	private boolean updateForeground(boolean playback, boolean microphone)
	{
		try
		{
			if (Build.VERSION.SDK_INT >= 29)
			{
				int types = Build.VERSION.SDK_INT >= 34 ? ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING : 0;
				if (playback)
				{
					types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
				}
				if (microphone && Build.VERSION.SDK_INT >= 30)
				{
					types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
				}
				this.startForeground(1, this.foregroundNotification, types);
			}
			else
			{
				this.startForeground(1, this.foregroundNotification);
			}
			this.foregroundAdmitted = true;
			return true;
		}
		catch (RuntimeException refused)
		{
			Log.w("lemonchat", "foreground service request refused", refused);
			return false;
		}
	}

	// job id of the restart job, any fixed number unique inside this app
	private static final int RESTART_JOB_ID = 1001;

	/**
	 * Registers the persisted periodic service restart job when it is not already scheduled.
	 */
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
	private volatile String activeCallId = "";
	private volatile String activeCallSession = "";
	private volatile long activeCallExpiry;

	/**
	 * Retires the displayed incoming call when its remote session is no longer connected.
	 * Submitted actions remain owned by NodeBridge until their scoped result or expiry.
	 */
	public synchronized void clearIncomingCall()
	{
		this.activeCallId = "";
		this.activeCallSession = "";
		this.activeCallExpiry = 0;
		if (this.notificationManager != null)
		{
			this.notificationManager.cancel(INCOMING_CALL_NOTIFICATION_ID);
		}
	}

	/**
	 * Checks whether a native action still belongs to the displayed unexpired call.
	 *
	 * @param callId call capability supplied by the native action
	 * @param sessionId session that owns the capability
	 * @return whether both identifiers match the current unexpired call
	 */
	public synchronized boolean isCurrentCall(String callId, String sessionId)
	{
		return callId != null && sessionId != null && !callId.isEmpty() && callId.equals(this.activeCallId)
			&& sessionId.equals(this.activeCallSession) && System.currentTimeMillis() < this.activeCallExpiry;
	}

	/**
	 * Shows a valid current call once with its Accept and Decline actions.
	 *
	 * @param callerName caller name displayed by Android
	 * @param channelid channel requested by the call and checked by Node
	 * @param callId unpredictable call capability
	 * @param sessionId logical session owning the call
	 * @param expiresAt absolute expiry time in milliseconds
	 */
	public synchronized void showIncomingCall(String callerName, int channelid, String callId, String sessionId, long expiresAt)
	{
		try
		{
			long now = System.currentTimeMillis();
			if (callId == null || !callId.matches("[a-fA-F0-9]{32}") || sessionId == null || sessionId.length() > 128
				|| sessionId.isEmpty() || expiresAt <= now || expiresAt > now + 120000 || this.isCurrentCall(callId, sessionId))
			{
				return;
			}
			this.activeCallId = callId;
			this.activeCallSession = sessionId;
			this.activeCallExpiry = expiresAt;

			//these intents get send to onStartCommand
			//basically app sends intents to itself

			// 3️⃣ Accept action PendingIntent → launches MainActivity, which forwards to the service.
			// android 12+ blocks a service tapped from a notification from starting an activity
			// (trampoline ban), so accepting used to run the js but never bring the app forward
			Intent acceptIntent = new Intent(this, MainActivity.class);
			acceptIntent.putExtra("call_id", callId);
			acceptIntent.putExtra("session_id", sessionId);
			acceptIntent.putExtra("expires_at", expiresAt);
			acceptIntent.setData(Uri.parse("lemonchat-call://accept/" + callId));
			acceptIntent.setAction(ACTION_ACCEPT_CALL);
			acceptIntent.putExtra("channelId", channelid);
			acceptIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
			PendingIntent acceptPendingIntent = PendingIntent.getActivity(this, 1, acceptIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			// 4️⃣ Decline action PendingIntent → handled by service
			Intent declineIntent = new Intent(this, BackgroundService.class);
			declineIntent.putExtra("call_id", callId);
			declineIntent.putExtra("session_id", sessionId);
			declineIntent.putExtra("expires_at", expiresAt);
			declineIntent.setData(Uri.parse("lemonchat-call://decline/" + callId));
			declineIntent.setAction(ACTION_DECLINE_CALL);
			PendingIntent declinePendingIntent = PendingIntent.getService(this, 2, declineIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			// 5️⃣ Full-screen intent → can be MainActivity (no custom CallActivity required)
			Intent fullScreenIntentActivity = new Intent(this, IncomingCallActivity.class);
			fullScreenIntentActivity.putExtra("call_id", callId);
			fullScreenIntentActivity.putExtra("session_id", sessionId);
			fullScreenIntentActivity.putExtra("expires_at", expiresAt);
			fullScreenIntentActivity.setData(Uri.parse("lemonchat-call://screen/" + callId));
			fullScreenIntentActivity.putExtra("channelId", channelid);
			fullScreenIntentActivity.putExtra("callerName", callerName);
			// NEW_TASK only: CLEAR_TOP pulled this into the chat's task, so declining revealed
			// the chat instead of returning the user to where they were
			fullScreenIntentActivity.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
			PendingIntent fullScreenIntent = PendingIntent.getActivity(this, 0, fullScreenIntentActivity, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

			androidx.core.app.Person caller = new androidx.core.app.Person.Builder().setName(callerName).setImportant(true).build();

			NotificationCompat.Builder callNotification = new NotificationCompat.Builder(this, CALL_NOTIFICATION_CHANNEL_ID)
							    .setContentIntent(fullScreenIntent)
							    .setContentText(callerName)
							    .setContentTitle("Incoming call")
							    .setAutoCancel(true)
							    .setOngoing(true) //cant be cancelled by swipe, only be accept / refuse
							    .setTimeoutAfter(15000)
							    .setPriority(NotificationCompat.PRIORITY_HIGH)
							    .setCategory(Notification.CATEGORY_CALL)
							    .setSound(this.callSoundUri)
							    .setSmallIcon(android.R.drawable.ic_menu_call) //wont work without this
							    .setVibrate(this.vibrationPattern)
							    .setStyle(NotificationCompat.CallStyle.forIncomingCall(caller, declinePendingIntent, acceptPendingIntent))
							    .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
							    .addPerson(caller);
			if (Build.VERSION.SDK_INT < 34 || this.notificationManager.canUseFullScreenIntent())
			{
				callNotification.setFullScreenIntent(fullScreenIntent, true);
			}
			Notification notification = callNotification.build();

			notification.flags |= NotificationCompat.FLAG_INSISTENT; //notification should persistent in the status bar until its dismissed

			this.notificationManager.notify(INCOMING_CALL_NOTIFICATION_ID, notification);
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}

	/**
	 * Displays a background message notification whose tap opens the chat activity.
	 *
	 * @param senderName title identifying the sender
	 * @param messageText notification body supplied by the bridge
	 */
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

	/**
	 * Displays a background poke notification whose tap opens the chat activity.
	 *
	 * @param senderName title identifying the sender
	 * @param pokeMessage poke text displayed by Android
	 */
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
