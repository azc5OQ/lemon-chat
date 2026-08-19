package com.lemonchat;

import static android.view.View.INVISIBLE;
import static android.view.View.VISIBLE;

import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import android.widget.Spinner;
import android.widget.Toast;

import java.util.ArrayList;

import com.google.android.material.switchmaterial.SwitchMaterial;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

public class MainActivity extends AppCompatActivity
{
	private BackgroundService backgroundService = null;

	private ChatSettings settings = null;

	public static MainActivity instance = null;

	SharedPreferences preferences = null;

	private Permissions permissions = null;

	public static int FILE_CHOOSER_RESULT_CODE = 987454;

	// guards the first-run mode dialog so a second permission callback cannot stack another copy on it
	private boolean isAppModeDialogVisible = false;

	// the permission run starts on the first onResume, not in onCreate, and only once per process
	private boolean hasBegunPermissionChain = false;

	@Override protected void onResume()
	{
		Log.d("Info", "[lemonchat] onResume");

		super.onResume();

		MainActivity.instance = this;

		// Move WebView back from service overlay (if it was there)

		try
		{
			this.handleOnResumeOnCreate();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onResume Exception: " + ex.getMessage());
			ex.printStackTrace();
		}

		//the activity is on screen now, so a system dialog has something to sit on. the guard keeps
		//every later resume (returning from the prompts themselves included) from restarting the run
		if (this.permissions != null && this.hasBegunPermissionChain == false)
		{
			this.hasBegunPermissionChain = true;
			this.permissions.beginCheck();
		}

		// no handover on resume: node keeps the server connection, the webview only re-renders
	}

	@Override protected void onPause()
	{
		try
		{
			this.handleOnStopOnDestroyOnPause();
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}

		super.onPause();
	}

	//fires when the call notification's accept button reuses an existing instance (single_top)
	@Override protected void onNewIntent(Intent intent)
	{
		super.onNewIntent(intent);

		this.forwardAcceptCallToService(intent);
	}

	//the accept action targets this activity because android 12+ forbids the old route of the
	//service starting an activity off a notification tap; the accept plumbing stays in the service
	private void forwardAcceptCallToService(Intent intent)
	{
		if (intent == null || BackgroundService.ACTION_ACCEPT_CALL.equals(intent.getAction()) == false)
		{
			return;
		}

		Intent acceptIntent = new Intent(this, BackgroundService.class);
		acceptIntent.setAction(BackgroundService.ACTION_ACCEPT_CALL);
		acceptIntent.putExtra("channelId", intent.getIntExtra("channelId", 0));
		this.startService(acceptIntent);

		//clear the action so a relaunch of this activity cannot re-accept the call
		intent.setAction(null);
	}

	@Override public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults)
	{
		super.onRequestPermissionsResult(requestCode, permissions, grantResults);

		// resume the permission chain instead of asking the first-run question here: this callback
		// fires after EVERY classic permission prompt (the notification one comes first!), so asking
		// directly popped the mode dialog in the middle of the permission run. only the microphone
		// request lands here without a launcher callback of its own - resuming for the others too
		// would run doCheck twice per result and double-launch the settings screens
		if (requestCode == Permissions.PERMISSIONS_RECORD_AUDIO_REQUEST_CODE)
		{
			this.permissions.resumeCheck();
		}
	}

	//onActivityResult for file chooser in WebView
	@Override protected void onActivityResult(int requestCode, int resultCode, Intent data)
	{
		super.onActivityResult(requestCode, resultCode, data);
		if (requestCode == FILE_CHOOSER_RESULT_CODE)
		{
			Log.d("Info", "[lemonchat] onActivityResult :");

			if (resultCode == RESULT_OK && this.backgroundService.fileChooserCallback != null)
			{
				Log.d("Info", "[lemonchat] onActivityResult :");

				Uri[] result = null;

				if (data != null)
				{
					String dataString = data.getDataString();
					if (dataString != null)
					{
						result = new Uri[] { Uri.parse(dataString) };
					}
				}

				this.backgroundService.fileChooserCallback.onReceiveValue(result);
			}
			else
			{
				//user cancelled or error occured
				//pass null to reset the state of the file input

				if (this.backgroundService.fileChooserCallback != null)
				{
					this.backgroundService.fileChooserCallback.onReceiveValue(null);
				}
			}

			this.backgroundService.fileChooserCallback = null;
		}
	}

	private void startLemonchatBackgroundService()
	{
		ServiceConnection connection = new ServiceConnection() {
			@Override public void onServiceConnected(ComponentName name, IBinder service)
			{
				BackgroundService.LocalBinder binder = (BackgroundService.LocalBinder)service;
				MainActivity.this.backgroundService = binder.getService();

				//do something

				MainActivity.this.handleOnResumeOnCreate();
			}

			@Override public void onServiceDisconnected(ComponentName name)
			{
			}
		};

		//create service

		Intent intent = new Intent(this, BackgroundService.class);
		//this.startService(serviceIntent); -> gets cancelled shortly

		if (!BackgroundService.isRunning)
		{
			this.startForegroundService(intent); //calls createWebViewInServiceContext
			Log.d("Info", "[lemonchat] this.startForegroundService(intent) :");
		}
		else
		{
			Log.d("Info", "[lemonchat] service already running !:");
		}

		this.bindService(intent, connection, Context.BIND_AUTO_CREATE); //this is what triggers onServiceConnected
	}

	private void handleOnStopOnDestroyOnPause()
	{
		MainActivity.instance = null;

		//nobody is looking any more, so node may go back to idle
		this.reportUiVisibility(false);

		Log.d("Info", "[lemonchat] handleOnStopOnDestroyOnPause");
		if (this.backgroundService != null)
		{
			// Move WebView back from service overlay (if it was there)
			if (backgroundService.webView != null)
			{
				if (backgroundService.isWebViewAttachedToHiddenWindow == false)
				{
					FrameLayout frameLayout = this.findViewById(R.id.root_container);

					if (frameLayout != null && this.backgroundService != null)
					{
						frameLayout.removeView(this.backgroundService.webView);
						this.backgroundService.attachWebViewToInvisibleWindow();
					}
				}
			}
		}
	}

	@Override protected void onCreate(Bundle savedInstanceState)
	{
		try
		{
			super.onCreate(savedInstanceState);
			setContentView(R.layout.main_activity);

			MainActivity.instance = this;

			this.preferences = this.getPreferences(Context.MODE_PRIVATE);
			this.settings = ChatSettings.getInstance();
			//ChatSettings resolves its own preferences from this context now, so the service can
			//read them at boot with no activity around
			ChatSettings.attachContext(this);

			this.ensureIdentityAndMode();

			//registering must happen here, but ASKING must not: a permission dialog launched from
			//onCreate has no started activity to attach to and the system drops it, which is why the
			//notification prompt only turned up on the second launch. onResume starts the chain.
			this.permissions = new Permissions(this);

			//	sometimes onCreate is called if service is not running, sometimes its called when service is already running
			//	handle both cases
			if (this.backgroundService == null)
			{
				this.startLemonchatBackgroundService();
			}
			else
			{
				this.handleOnResumeOnCreate();
			}

			//the notification's accept button may have created this activity from scratch
			this.forwardAcceptCallToService(this.getIntent());

			try
			{
				LinearLayout keysContainer = findViewById(R.id.keys_container);

				// Optional: add one key field by default

				if (keysContainer.getChildCount() == 0)
				{
					Button addKeyBtn = findViewById(R.id.add_key);
					addKeyBtn.setOnClickListener(v -> this.addKeyField(null));
					this.addKeyField(null);
				}

				this.wireServerBookmarks();
			}
			catch (Exception ex)
			{
				ex.printStackTrace();
			}
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onCreate" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	public void showSettings()
	{
		FrameLayout frameLayout = this.findViewById(R.id.root_container);

		if (frameLayout != null && this.backgroundService != null)
		{
			frameLayout.removeView(this.backgroundService.webView);
			this.backgroundService.attachWebViewToInvisibleWindow();
		}

		// repopulate the fields from the persisted json EVERY time the panel opens. the foreground
		// service keeps the process alive across an app "close", so a relaunch builds a fresh
		// activity with blank fields while the one-shot LoadSettings of this process already ran
		// in the previous activity - saved settings looked lost even though they were on disk
		this.LoadSettings();

		this.findViewById(R.id.settings_panel).setVisibility(VISIBLE);
	}

	//saved server configurations, kept beside the settings json under their own preference key so
	//that saving settings can never disturb them: [{ name, host, port, metadata_keys }]
	private JSONArray loadServerBookmarks()
	{
		try { return new JSONArray(this.preferences.getString("server_bookmarks", "[]")); }
		catch (Exception ex) { return new JSONArray(); }
	}

	private void storeServerBookmarks(JSONArray bookmarks)
	{
		this.preferences.edit().putString("server_bookmarks", bookmarks.toString()).apply();
	}

	//row 0 is the placeholder, so bookmark N is row N + 1
	private void refreshServerBookmarkSpinner()
	{
		Spinner spinner = this.findViewById(R.id.bookmark_spinner);

		if (spinner == null)
		{
			return;
		}

		JSONArray bookmarks = this.loadServerBookmarks();
		ArrayList<String> labels = new ArrayList<>();
		labels.add(bookmarks.length() > 0 ? "saved servers" : "no saved servers");

		for (int i = 0; i < bookmarks.length(); i++)
		{
			JSONObject bookmark = bookmarks.optJSONObject(i);

			if (bookmark == null)
			{
				continue;
			}

			labels.add(bookmark.optString("name") + " (" + bookmark.optString("host") + ":" + bookmark.optInt("port") + ")");
		}

		ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
		adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
		spinner.setAdapter(adapter);
	}

	//the key rows currently on screen, which belong to whichever server is in the fields
	private JSONArray readKeyFields()
	{
		JSONArray keysArray = new JSONArray();
		LinearLayout keysContainer = this.findViewById(R.id.keys_container);

		for (int i = 0; i < keysContainer.getChildCount(); i++)
		{
			EditText keyInput = keysContainer.getChildAt(i).findViewById(R.id.key_input);
			String key = keyInput.getText().toString().trim();

			if (key.isEmpty() == false)
			{
				keysArray.put(key);
			}
		}

		return keysArray;
	}

	private void wireServerBookmarks()
	{
		Spinner spinner = this.findViewById(R.id.bookmark_spinner);

		if (spinner == null)
		{
			return;
		}

		this.refreshServerBookmarkSpinner();

		spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
		{
			@Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
			{
				if (position == 0)
				{
					return;
				}

				JSONObject bookmark = MainActivity.this.loadServerBookmarks().optJSONObject(position - 1);

				if (bookmark == null)
				{
					return;
				}

				((EditText) MainActivity.this.findViewById(R.id.ip_address)).setText(bookmark.optString("host"));
				((EditText) MainActivity.this.findViewById(R.id.websocket_port)).setText(String.valueOf(bookmark.optInt("port")));
				((EditText) MainActivity.this.findViewById(R.id.bookmark_name)).setText(bookmark.optString("name"));

				//the metadata keys are part of the server, so they travel with it
				LinearLayout keysContainer = MainActivity.this.findViewById(R.id.keys_container);
				keysContainer.removeAllViews();

				JSONArray keys = bookmark.optJSONArray("metadata_keys");

				if (keys != null)
				{
					for (int i = 0; i < keys.length(); i++)
					{
						String value = keys.optString(i, "");

						if (value.isEmpty() == false)
						{
							MainActivity.this.addKeyField(value);
						}
					}
				}

				//a server with no keys still needs the one empty row the screen normally starts with
				if (keysContainer.getChildCount() == 0)
				{
					MainActivity.this.addKeyField(null);
				}
			}

			@Override public void onNothingSelected(AdapterView<?> parent) { }
		});

		this.findViewById(R.id.bookmark_save).setOnClickListener(v -> this.saveCurrentServerAsBookmark());
		this.findViewById(R.id.bookmark_delete).setOnClickListener(v -> this.deleteSelectedServerBookmark());
	}

	private void saveCurrentServerAsBookmark()
	{
		String name = ((EditText) this.findViewById(R.id.bookmark_name)).getText().toString().trim();
		String host = ((EditText) this.findViewById(R.id.ip_address)).getText().toString().trim();
		String port = ((EditText) this.findViewById(R.id.websocket_port)).getText().toString().trim();

		if (name.isEmpty())
		{
			Toast.makeText(this, "name this server first", Toast.LENGTH_SHORT).show();
			return;
		}

		if (host.isEmpty())
		{
			Toast.makeText(this, "fill in the address first", Toast.LENGTH_SHORT).show();
			return;
		}

		int portNumber = 0;
		try { portNumber = Integer.parseInt(port); } catch (Exception ex) { }

		try
		{
			JSONObject entry = new JSONObject();
			entry.put("name", name);
			entry.put("host", host);
			entry.put("port", portNumber);
			entry.put("metadata_keys", this.readKeyFields());

			JSONArray bookmarks = this.loadServerBookmarks();
			int existing = -1;

			for (int i = 0; i < bookmarks.length(); i++)
			{
				JSONObject stored = bookmarks.optJSONObject(i);

				if (stored != null && stored.optString("name").equalsIgnoreCase(name))
				{
					existing = i;
				}
			}

			//saving under a name that already exists overwrites it, so this doubles as "update"
			if (existing == -1) { bookmarks.put(entry); }
			else { bookmarks.put(existing, entry); }

			this.storeServerBookmarks(bookmarks);
			this.refreshServerBookmarkSpinner();

			Toast.makeText(this, "saved " + name, Toast.LENGTH_SHORT).show();
		}
		catch (Exception ex)
		{
			Log.e("lemonchat", "saving a server bookmark failed", ex);
		}
	}

	private void deleteSelectedServerBookmark()
	{
		Spinner spinner = this.findViewById(R.id.bookmark_spinner);
		int position = spinner.getSelectedItemPosition();

		if (position <= 0)
		{
			Toast.makeText(this, "pick a saved server to delete", Toast.LENGTH_SHORT).show();
			return;
		}

		JSONArray bookmarks = this.loadServerBookmarks();
		JSONArray remaining = new JSONArray();

		for (int i = 0; i < bookmarks.length(); i++)
		{
			if (i != position - 1)
			{
				remaining.put(bookmarks.opt(i));
			}
		}

		this.storeServerBookmarks(remaining);
		this.refreshServerBookmarkSpinner();

		((EditText) this.findViewById(R.id.bookmark_name)).setText("");
	}

	// Add a key row (inflated from key_field_row.xml: outlined password field + remove button)
	private void addKeyField(String valueOfThatKeyField)
	{
		LinearLayout keysContainer = findViewById(R.id.keys_container);

		View row = getLayoutInflater().inflate(R.layout.key_field_row, keysContainer, false);

		EditText keyInput = row.findViewById(R.id.key_input);

		if (valueOfThatKeyField != null)
		{
			keyInput.setText(valueOfThatKeyField);
		}

		// removing every key row is fine - servers can run without metadata keys
		row.findViewById(R.id.remove_key).setOnClickListener(v -> keysContainer.removeView(row));

		keysContainer.addView(row);
	}

	//app-held identity + first-run mode question. the identity seed is generated once and kept in
	//SharedPreferences (with allowBackup it survives reinstalls); both values are merged into the
	//stored settings json so the web client receives them with the rest of the settings
	private void ensureIdentityAndMode()
	{
		String identitySeed = this.preferences.getString("identity_seed", "");

		if (identitySeed.equals(""))
		{
			identitySeed = ChatSettings.generateIdentitySeed();
			this.preferences.edit().putString("identity_seed", identitySeed).apply();
		}

		this.mergeFieldIntoStoredSettings("identity_string", identitySeed);

		// a mode that was already chosen just rides along into the settings json. the first-run
		// QUESTION is not asked here: it used to be, and it opened underneath the android permission
		// prompts that start right after this - the flashing seen on some devices.
		// promptForAppModeIfNeeded() asks once the permission run is over.
		String appMode = this.preferences.getString("app_mode", "");

		if (appMode.equals("") == false)
		{
			this.mergeFieldIntoStoredSettings("app_mode", appMode);
		}
	}

	//adds/updates one field of the persisted settings json. it CREATES the json when none exists
	//yet: the web client only ever learns the app mode (and with it, that simple mode wants the
	//normie look) from these settings, so a user who picks a mode on first run but never opens the
	//settings screen used to stay on the default theme forever.
	private void mergeFieldIntoStoredSettings(String fieldName, String value)
	{
		try
		{
			String json = this.preferences.getString("settings", "");
			JSONObject settingsJson = (json.equals("") == false) ? new JSONObject(json) : new JSONObject();

			if (value.equals(settingsJson.optString(fieldName, "")) == false)
			{
				settingsJson.put(fieldName, value);
				this.settings.saveJsonSettings(settingsJson.toString());
			}
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}
	}

	// asked once, and only after the android permission prompts are finished, so our dialog is never
	// buried under a system one. safe to call repeatedly: it returns immediately once a mode is stored,
	// while the dialog is already up, or while the activity is going away.
	public void promptForAppModeIfNeeded()
	{
		if (this.preferences == null || this.isAppModeDialogVisible || this.isFinishing())
		{
			return;
		}

		if (this.preferences.getString("app_mode", "").equals("") == false)
		{
			return;
		}

		this.showFirstRunModeDialog();
	}

	private void showFirstRunModeDialog()
	{
		this.isAppModeDialogVisible = true;

		new AlertDialog.Builder(this)
			.setTitle("Choose mode")
			.setMessage("Simple: a plain messenger look, set up with defaults.\n\nAdvanced: the full interface with channels and voice controls.\n\nCan be changed later in settings.")
			.setCancelable(false)
			.setPositiveButton("Simple", (dialog, which) -> this.storeAppMode("simple"))
			.setNegativeButton("Advanced", (dialog, which) -> this.storeAppMode("advanced"))
			.show();
	}

	private void storeAppMode(String mode)
	{
		this.isAppModeDialogVisible = false;
		this.preferences.edit().putString("app_mode", mode).apply();
		this.mergeFieldIntoStoredSettings("app_mode", mode);

		// push the choice into the already-loaded page right away - without this the first-run
		// answer only took effect on the NEXT launch (the page asks for settings before the
		// dialog is answered), so "simple" appeared to do nothing
		if (this.backgroundService != null && this.backgroundService.javascriptJavaBridge != null)
		{
			this.backgroundService.javascriptJavaBridge.JavaExportRequestCurrentSettingsFromAndroid();
		}
	}

	//tells node if the user is looking at the app. being connected is not the same as being on
	//screen, and mixing the two up is why opening the app used to leave the client idle
	private void reportUiVisibility(boolean isVisible)
	{
		if (this.backgroundService == null || this.backgroundService.nodeBridge == null)
		{
			return;
		}

		//a file picker is another app sitting on top of ours, not the user walking away.
		//without this check, picking a file would send the client to idle
		if (isVisible == false && this.backgroundService.fileChooserCallback != null)
		{
			return;
		}

		//the user just declined a call, so he should stay idle. the flag is cleared further down
		//in this same resume, so we look at it here and leave the clearing to that code
		if (isVisible == true && BackgroundService.suppressNextIdleExit)
		{
			return;
		}

		this.backgroundService.nodeBridge.sendUiVisible(isVisible);
	}

	public void handleOnResumeOnCreate()
	{
		//both entry points: resume, and service connect for when the activity beat the binding
		this.reportUiVisibility(true);

		//check if WebView is attached to invisible window of background service
		if (this.backgroundService != null)
		{
			if (this.backgroundService.webView != null)
			{
				if (this.backgroundService.isWebViewAttachedToHiddenWindow == true)
				{
					FrameLayout frameLayout = this.findViewById(R.id.root_container);

					if (frameLayout != null)
					{
						this.backgroundService.detachWebViewFromInvisibleWindow();

						//layout params for frameLayout are needed, without them the WebView appears as black
						WindowManager.LayoutParams params = null;

						if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
						{
							params = new WindowManager.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT,
												WindowManager.LayoutParams.TYPE_APPLICATION, //or is it TYPE_APPLICATION_PANEL
												WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, PixelFormat.TRANSLUCENT);
						}
						else
						{
							params = new WindowManager.LayoutParams(android.view.ViewGroup.LayoutParams.WRAP_CONTENT, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.TYPE_PHONE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, PixelFormat.TRANSLUCENT);
						}

						frameLayout.addView(this.backgroundService.webView, params);
					}
				}
				else
				{
					//the app was started and there is no attached window, probably first start
					Log.d("Info", "[lemonchat] app started for first time");
					FrameLayout frameLayout = this.findViewById(R.id.root_container);

					if (!(this.backgroundService.webView.getParent() instanceof ViewGroup))
					{
						frameLayout.addView(this.backgroundService.webView);
					}
				}

				//a just-declined call sets this flag: skip the idle exit ONCE so declining does
				//not yank the user out of idle into the root channel
				if (BackgroundService.suppressNextIdleExit)
				{
					BackgroundService.suppressNextIdleExit = false;
				}
				else
				{
					// typeof guard: this can fire while the page is still loading, where a bare
					// call is a ReferenceError that evaluateJavascript swallows silently
					this.backgroundService.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_come_from_idle_mode_request === 'function') { JavascriptJavaBridge__send_come_from_idle_mode_request(); }", null);
				}
			}
		}
	}

	@Override protected void onStop()
	{
		try
		{
			Log.d("Info", "[lemonchat] onStop");
			this.handleOnStopOnDestroyOnPause();

			// node owns the server connection at all times; backgrounding only asks for idle mode,
			// and that request rides the loopback through node like any other.
			// the file picker is another app on top of ours, so this fired and dropped the user
			// into idle just for opening it - a pending chooser callback means we are still "in"
			if (this.backgroundService != null && this.backgroundService.webView != null
				&& this.backgroundService.fileChooserCallback == null)
			{
				this.backgroundService.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_go_to_idle_mode_request === 'function') { JavascriptJavaBridge__send_go_to_idle_mode_request(); }", null);
			}

			super.onStop();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onCreate" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	@Override protected void onDestroy()
	{
		try
		{
			Log.d("Info", "[lemonchat] onDestroy");

			//the window is closing, so a file picker it opened will never answer. cleared before
			//the line below, which refuses to report anything while a picker is remembered
			if (this.backgroundService != null)
			{
				this.backgroundService.abandonPendingFileChooser();
			}

			this.handleOnStopOnDestroyOnPause();
			this.backgroundService.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_go_to_idle_mode_request === 'function') { JavascriptJavaBridge__send_go_to_idle_mode_request(); }", null);

			super.onDestroy();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onDestroy" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	//invoked by
	// @JavascriptInterface public void JavaExportRequestCurrentSettingsFromAndroid()
	public boolean LoadSettings()
	{
		boolean result = false;

		try
		{
			String json = this.preferences.getString("settings", "");

			this.settings.saveJsonSettings(json);

			if (json.equals("") == false)
			{
				Log.d("Info", "[lemonchat] settings string is not empty");

				JSONObject settingsJson = new JSONObject(json);

				// opt* with defaults, never the throwing getters: a json missing ONE newer field
				// (partial first-run json, or one saved by an older app version) used to abort this
				// whole method - every settings field stayed blank even though the values were on
				// disk, and one tap of "save" then overwrote the good json with those blanks
				String ipAddress = settingsJson.optString("host", "");
				int websocketPort = settingsJson.optInt("port", 0);
				String defaultUsername = settingsJson.optString("default_username", "");
				boolean isMicAlwaysOnEnabled = settingsJson.optBoolean("is_microphone_always_on", false);
				boolean isAutoconnectEnabled = settingsJson.optBoolean("is_autoconnect_enabled", true);
				boolean isAudioEffectDisabled = settingsJson.optBoolean("is_audio_effect_enabled", false);
				boolean isAppLogEnabled = settingsJson.optBoolean("is_app_log_enabled", false);

				LinearLayout keysContainerLinearLayout = findViewById(R.id.keys_container);
				EditText ipAddressEditText = findViewById(R.id.ip_address);
				EditText portEditText = findViewById(R.id.websocket_port);
				EditText defaultUsernameEditText = findViewById(R.id.default_username);
				SwitchMaterial autoConnectSwitch = findViewById(R.id.settings_autoconnect_enabled);

				ipAddressEditText.setText(ipAddress);
				portEditText.setText(String.valueOf(websocketPort));
				defaultUsernameEditText.setText(defaultUsername);

				//mic mode, sound effects and the app-log toggle live in the client's local settings
				//now; their stored values are carried through the save untouched
				autoConnectSwitch.setChecked(isAutoconnectEnabled);

                //clear out keys first
                LinearLayout keysContainer = findViewById(R.id.keys_container);
                keysContainer.removeAllViews();

                JSONArray keys = settingsJson.optJSONArray("metadata_keys");

				if (keys != null)
				{
					for (int i = 0; i < keys.length(); i++)
					{
						String value = keys.optString(i, "");
						if (!value.isEmpty())
						{
							this.addKeyField(value);
						}
					}
				}

				//the saved list can have grown since the screen was last built
				this.refreshServerBookmarkSpinner();

				this.settings.setDefaultUsername(defaultUsername);
				this.settings.setWebsocketPort(websocketPort);
				this.settings.setAreAudioEffectsEnabled(!isAudioEffectDisabled);
				this.settings.setContinousAudioBroadcastEnabled(isMicAlwaysOnEnabled);
			}

			result = true;
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] LoadSettings exception:" + ex.getMessage());
			ex.printStackTrace();
		}

		return result;
	}

	// hides the settings panel and brings the chat webview back to the foreground. shared by Save
	// and by the Back button - the ONLY difference is Back does not persist the fields
	private void restoreWebViewToForeground()
	{
		FrameLayout frameLayout = this.findViewById(R.id.root_container);

		this.findViewById(R.id.settings_panel).setVisibility(INVISIBLE);

		if (frameLayout != null && this.backgroundService != null)
		{
			frameLayout.removeView(this.backgroundService.webView);

			this.backgroundService.detachWebViewFromInvisibleWindow();

			//layout params for frameLayout are needed, without them the WebView appears as black
			WindowManager.LayoutParams params = null;

			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
			{
				params = new WindowManager.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT,
									WindowManager.LayoutParams.TYPE_APPLICATION, //or is it TYPE_APPLICATION_PANEL
									WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, PixelFormat.TRANSLUCENT);
			}
			else
			{
				params = new WindowManager.LayoutParams(android.view.ViewGroup.LayoutParams.WRAP_CONTENT, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.TYPE_PHONE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, PixelFormat.TRANSLUCENT);
			}

			frameLayout.addView(this.backgroundService.webView, params);
		}
	}

	// Back button: leaves settings and returns to chat WITHOUT saving. Save was the only way out
	public void closeSettings(View view)
	{
		this.restoreWebViewToForeground();
	}

	public void saveSettings(View view)
	{
		// read the form BEFORE restoring the webview - reading after the teardown is what a
		// half-saved settings screen looks like

		// Get UI values
		LinearLayout keysContainer = findViewById(R.id.keys_container);
		EditText ipAddress = findViewById(R.id.ip_address);
		EditText port = findViewById(R.id.websocket_port);
		EditText defaultUsername = findViewById(R.id.default_username);
		SwitchMaterial autoConnect = findViewById(R.id.settings_autoconnect_enabled);

		String ip = ipAddress.getText().toString().trim();
		String defaultUsernameString = defaultUsername.getText().toString().trim();

		int portStr = 0;
		try
		{
			portStr = Integer.parseInt(port.getText().toString().trim());
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}

		//these three moved into the client's local settings; carry the stored values through
		//untouched so saving connection details cannot silently reset them
		JSONObject previousSettings = null;
		try { previousSettings = new JSONObject(this.settings.getJsonSettings()); }
		catch (Exception noPrevious) { previousSettings = new JSONObject(); }

		boolean isContinousAudioBroadcastEnabled = previousSettings.optBoolean("is_microphone_always_on", false);
		boolean isAudioEffectsEnabled = previousSettings.optBoolean("is_audio_effect_enabled", false);
		boolean isAppLogEnabled = previousSettings.optBoolean("is_app_log_enabled", false);
		boolean isAutoconnectEnabled = autoConnect.isChecked();

		// Create the master JSONObject
		JSONObject settingsJson = new JSONObject();

		try
		{
			// Put static values
			settingsJson.put("host", ip);
			settingsJson.put("port", portStr);
			settingsJson.put("default_username", defaultUsernameString);
			settingsJson.put("is_microphone_always_on", isContinousAudioBroadcastEnabled);
			settingsJson.put("is_autoconnect_enabled", isAutoconnectEnabled);
			settingsJson.put("is_audio_effect_enabled", isAudioEffectsEnabled);
			settingsJson.put("is_app_log_enabled", isAppLogEnabled);

			//app-held identity and mode ride along with every save
			settingsJson.put("identity_string", this.preferences.getString("identity_seed", ""));
			settingsJson.put("app_mode", this.preferences.getString("app_mode", "advanced"));

			// Create a JSONArray for the dynamic keys. servers may run with no metadata keys at all,
			// so an empty row is simply skipped instead of failing the whole save - it just means
			// "no key here". a server that does use keys rejects the connection on its own.
			JSONArray keysArray = new JSONArray();
			for (int i = 0; i < keysContainer.getChildCount(); i++)
			{
				View row = keysContainer.getChildAt(i);
				EditText keyInput = row.findViewById(R.id.key_input);
				String key = keyInput.getText().toString().trim();
				if (!key.isEmpty())
				{
					keysArray.put(key);
				}
			}

			settingsJson.put("metadata_keys", keysArray);

			this.settings.saveJsonSettings(settingsJson.toString());

			// pushing to node and the webview is the save hook's job (ChatSettings.onSettingsSaved)
		}
		catch (Exception e)
		{
			// Exception, not JSONException: an NPE here used to silently discard the whole save
			Log.e("lemonchat", "saveSettings failed", e);
		}

		this.restoreWebViewToForeground();
	}
}
