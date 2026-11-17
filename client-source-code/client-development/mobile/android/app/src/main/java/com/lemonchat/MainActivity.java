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
	private ServiceConnection serviceConnection;
	private boolean serviceBound;
	private boolean resumed;
	private boolean destroyed;
	private Intent pendingCallIntent;
	private static final int CONNECTION_WIZARD = 208;
	private boolean connectionWizardOpen;
	private boolean firstConnectionOffered;
	private boolean pendingWizardConnect;
	private boolean pendingAdvancedSettings;
	private boolean nativeSettingsOpen;

	private ChatSettings settings = null;

	public static volatile MainActivity instance = null;

	SharedPreferences preferences = null;

	private Permissions permissions = null;


	// guards the first-run mode dialog so a second permission callback cannot stack another copy on it
	private boolean isAppModeDialogVisible = false;

	// offer first setup after the activity becomes visible
	private boolean hasBegunPermissionChain = false;

	/**
	 * restores the visible page and offers first setup
	 */
	@Override protected void onResume()
	{
		Log.d("Info", "[lemonchat] onResume");

		super.onResume();

		MainActivity.instance = this;
		this.resumed = true;
		if (this.backgroundService != null)
		{
			this.backgroundService.completeAudioPermission(this);
		}

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

		// do not reopen setup after returning from another screen
		if (this.permissions != null && this.hasBegunPermissionChain == false)
		{
			this.hasBegunPermissionChain = true;
			this.permissions.beginCheck();
		}
		this.connectAfterWizard();
		if (this.pendingAdvancedSettings)
		{
			this.pendingAdvancedSettings = false;
			this.showSettings();
		}

		// no handover on resume: node keeps the server connection, the webview only re-renders
	}

	/**
	 * Releases foreground UI ownership while the Node runtime keeps its remote connection.
	 */
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

	/**
	 * Forwards a new call acceptance intent when Android reuses this activity.
	 *
	 * @param intent replacement activity intent
	 */
	@Override protected void onNewIntent(Intent intent)
	{
		super.onNewIntent(intent);

		this.setIntent(intent);
		this.forwardAcceptCallToService(intent);
	}

	/**
	 * Forwards an Accept capability before normal foreground presence reconciliation.
	 * If binding has not completed, the action waits for the service connection.
	 *
	 * @param intent activity intent that may contain an Accept action
	 */
	private void forwardAcceptCallToService(Intent intent)
	{
		if (intent == null || BackgroundService.ACTION_ACCEPT_CALL.equals(intent.getAction()) == false)
		{
			return;
		}

		Intent acceptIntent = new Intent(this, BackgroundService.class);
		acceptIntent.setAction(BackgroundService.ACTION_ACCEPT_CALL);
		acceptIntent.putExtra("call_id", intent.getStringExtra("call_id"));
		acceptIntent.putExtra("session_id", intent.getStringExtra("session_id"));
		if (this.backgroundService == null)
		{
			this.pendingCallIntent = acceptIntent;
		}
		else
		{
			this.backgroundService.handleCallAction(acceptIntent);
		}

		//clear the action so a relaunch of this activity cannot re-accept the call
		intent.setAction(null);
	}

	/**
	 * Routes an Android permission result to its pending page request or startup permission sequence.
	 *
	 * @param requestCode originating permission request code
	 * @param permissions permissions included in the result
	 * @param grantResults Android grant or denial results
	 */
	@Override public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults)
	{
		super.onRequestPermissionsResult(requestCode, permissions, grantResults);
		if (requestCode == 101 && this.backgroundService != null)
		{
			this.backgroundService.completeAudioPermission(this);
		}

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

	/**
	 * Routes a file picker result to the service that owns the current page callback.
	 *
	 * @param requestCode originating activity request code
	 * @param resultCode Android result status
	 * @param data returned picker intent, which may be {@code null}
	 */
	@Override protected void onActivityResult(int requestCode, int resultCode, Intent data)
	{
		super.onActivityResult(requestCode, resultCode, data);
		if (requestCode == CONNECTION_WIZARD)
		{
			this.connectionWizardOpen = false;
			if (resultCode == RESULT_OK)
			{
				this.pendingWizardConnect = true;
				this.connectAfterWizard();
			}
			else if (resultCode == ConnectionWizardActivity.ADVANCED_SETTINGS)
			{
				this.pendingAdvancedSettings = true;
			}
			return;
		}
		if (this.backgroundService != null)
		{
			this.backgroundService.completeFileChooser(this, requestCode, resultCode, data);
		}
	}

	/**
	 * Starts the foreground service when needed and binds this activity to its existing runtime.
	 */
	private void startLemonchatBackgroundService()
	{
		this.serviceConnection = new ServiceConnection()
		{
			@Override public void onServiceConnected(ComponentName name, IBinder service)
			{
				if (MainActivity.this.destroyed)
				{
					return;
				}
				BackgroundService.LocalBinder binder = (BackgroundService.LocalBinder)service;
				MainActivity.this.backgroundService = binder.getService();
				MainActivity.this.connectAfterWizard();
				if (MainActivity.this.pendingCallIntent != null)
				{
					MainActivity.this.backgroundService.handleCallAction(MainActivity.this.pendingCallIntent);
					MainActivity.this.pendingCallIntent = null;
				}

				//do something

				if (!MainActivity.this.destroyed && MainActivity.this.resumed)
				{
					MainActivity.this.handleOnResumeOnCreate();
				}
				else
				{
					MainActivity.this.reportUiVisibility(false);
				}
			}

			@Override public void onServiceDisconnected(ComponentName name)
			{
				MainActivity.this.backgroundService = null;
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

		this.serviceBound = this.bindService(intent, this.serviceConnection, Context.BIND_AUTO_CREATE); //this is what triggers onServiceConnected
	}

	/**
	 * Releases this activity's visibility and detaches its WebView without affecting a newer activity.
	 */
	private void handleOnStopOnDestroyOnPause()
	{
		this.resumed = false;
		if (MainActivity.instance != null && MainActivity.instance != this)
		{
			return;
		}
		if (MainActivity.instance == this)
		{
			MainActivity.instance = null;
		}

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

						// release the activity
						this.backgroundService.setWebViewWindowOwner(null);
						this.backgroundService.attachWebViewToInvisibleWindow();
					}
				}
			}
		}
	}

	/**
	 * Initializes the activity, native settings and service binding before the first resume.
	 *
	 * @param savedInstanceState framework state from a previous activity instance, if available
	 */
	@Override protected void onCreate(Bundle savedInstanceState)
	{
		try
		{
			super.onCreate(savedInstanceState);
			if (savedInstanceState != null)
			{
				this.firstConnectionOffered = savedInstanceState.getBoolean("connection_setup_offered");
				this.connectionWizardOpen = savedInstanceState.getBoolean("connection_setup_open");
				this.pendingWizardConnect = savedInstanceState.getBoolean("connection_setup_connect");
				this.pendingAdvancedSettings = savedInstanceState.getBoolean("connection_setup_advanced");
			}
			setContentView(R.layout.main_activity);
			HybridWindowInsets.apply(this, findViewById(R.id.main));

			//named file, NOT getPreferences(): since the launcher aliases, getPreferences() names
			//the file after the launching alias and split the settings away from ChatSettings
			this.preferences = this.getSharedPreferences("MainActivity", Context.MODE_PRIVATE);
			this.settings = ChatSettings.getInstance();
			//ChatSettings resolves its own preferences from this context now, so the service can
			//read them at boot with no activity around
			ChatSettings.attachContext(this);

			this.ensureIdentityAndMode();

			// register result callbacks before the activity starts
			this.permissions = new Permissions(this);
			this.permissions.restoreState(savedInstanceState);
			View callAlerts = this.findViewById(R.id.settings_call_alerts);
			callAlerts.setVisibility(Build.VERSION.SDK_INT >= 34 ? View.VISIBLE : View.GONE);
			callAlerts.setOnClickListener(view ->
			{
				if (Build.VERSION.SDK_INT >= 34)
				{
					this.startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_USE_FULL_SCREEN_INTENT, Uri.parse("package:" + this.getPackageName())));
				}
			});

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

	@Override protected void onSaveInstanceState(@NonNull Bundle state)
	{
		if (this.permissions != null)
		{
			this.permissions.saveState(state);
		}
		state.putBoolean("connection_setup_offered", this.firstConnectionOffered);
		state.putBoolean("connection_setup_open", this.connectionWizardOpen);
		state.putBoolean("connection_setup_connect", this.pendingWizardConnect);
		state.putBoolean("connection_setup_advanced", this.pendingAdvancedSettings);
		super.onSaveInstanceState(state);
	}

	/**
	 * opens the optional walkthrough from the settings form
	 *
	 * @param view the setup button
	 */
	public void runConnectionSetup(View view)
	{
		this.showConnectionWizard();
	}

	/** opens a separate connection draft without changing saved settings */
	public void showConnectionWizard()
	{
		if (this.connectionWizardOpen || this.destroyed || this.isFinishing())
		{
			return;
		}
		this.firstConnectionOffered = true;
		this.connectionWizardOpen = true;
		this.startActivityForResult(new Intent(this, ConnectionWizardActivity.class), CONNECTION_WIZARD);
	}

	/** sends the explicit Connect action after the service is available */
	private void connectAfterWizard()
	{
		if (this.pendingWizardConnect && this.resumed && !this.destroyed && this.backgroundService != null && this.backgroundService.nodeBridge != null)
		{
			if (this.permissions != null && !this.permissions.prepareConnection())
			{
				return;
			}
			this.pendingWizardConnect = false;
			this.restoreWebViewToForeground();
			this.backgroundService.nodeBridge.sendConnect();
		}
	}

	/** continues an explicit connection after its optional permission prompts */
	public void resumePendingConnection()
	{
		this.connectAfterWizard();
	}

	/** queues an explicit page connection until this activity can present permission prompts */
	public void requestConnection()
	{
		this.pendingWizardConnect = true;
		this.connectAfterWizard();
	}

	/**
	 * checks whether this activity can admit microphone access
	 *
	 * @return whether this activity is the visible owner
	 */
	public boolean isMediaActivityVisible()
	{
		return this.resumed && !this.destroyed && !this.isFinishing() && MainActivity.instance == this;
	}

	private void offerFirstConnection()
	{
		if (!this.firstConnectionOffered && this.settings != null && this.settings.getInitializationError().isEmpty() && this.settings.getIpAddress().isEmpty())
		{
			this.showConnectionWizard();
		}
	}

	/**
	 * Loads the latest committed settings into the native form and shows that panel.
	 */
	public void showSettings()
	{
		this.nativeSettingsOpen = true;
		FrameLayout frameLayout = this.findViewById(R.id.root_container);

		if (frameLayout != null && this.backgroundService != null)
		{
			frameLayout.removeView(this.backgroundService.webView);

			// release the activity
			this.backgroundService.setWebViewWindowOwner(null);
			this.backgroundService.attachWebViewToInvisibleWindow();
		}

		// repopulate the fields from the persisted json EVERY time the panel opens. the foreground
		// service keeps the process alive across an app "close", so a relaunch builds a fresh
		// activity with blank fields while the one-shot LoadSettings of this process already ran
		// in the previous activity - saved settings looked lost even though they were on disk
		this.LoadSettings();

		// no setup while connected
		boolean isServerConnected = this.backgroundService != null && this.backgroundService.nodeBridge != null && this.backgroundService.nodeBridge.isServerConnected;
		this.findViewById(R.id.run_connection_setup).setVisibility(isServerConnected ? android.view.View.GONE : VISIBLE);

		this.findViewById(R.id.settings_panel).setVisibility(VISIBLE);
	}

	/**
	 * Reads saved server profiles from their separate preference entry.
	 *
	 * @return saved profiles, or an empty array if the stored value cannot be parsed
	 */
	private JSONArray loadServerBookmarks()
	{
		try { return new JSONArray(this.preferences.getString("server_bookmarks", "[]")); }
		catch (Exception ex) { return new JSONArray(); }
	}

	/**
	 * Schedules persistence of the server profiles without replacing connection settings.
	 *
	 * @param bookmarks complete profile array to store
	 */
	private void storeServerBookmarks(JSONArray bookmarks)
	{
		this.preferences.edit().putString("server_bookmarks", bookmarks.toString()).apply();
	}

	/**
	 * Rebuilds the profile selector with a placeholder at row zero.
	 */
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

	/**
	 * reads server keys in their displayed order
	 *
	 * @return the ordered metadata-key array represented by the form
	 * @throws IllegalArgumentException when a key needs correction
	 */
	private JSONArray readKeyFields()
	{
		JSONArray keysArray = new JSONArray();
		LinearLayout keysContainer = this.findViewById(R.id.keys_container);

		for (int i = 0; i < keysContainer.getChildCount(); i++)
		{
			View row = keysContainer.getChildAt(i);
			EditText keyInput = row.findViewById(R.id.key_input);
			Spinner cipher = row.findViewById(R.id.key_type);
			int selected = cipher.getSelectedItemPosition();
			String type = selected >= 0 && selected < MetadataKey.TYPES.size() ? MetadataKey.TYPES.get(selected) : "";
			JSONObject key = MetadataKey.encode(keyInput.getTag(), type, keyInput.getText().toString());
			if (key != null)
			{
				keysArray.put(key);
			}
		}

		return keysArray;
	}

	/**
	 * Installs native profile selection, save and delete controls.
	 */
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
						Object value = keys.opt(i);

						if (value != null)
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

	/**
	 * Saves the host, port and metadata keys currently shown in the native form as a profile.
	 */
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
			Toast.makeText(this, ex instanceof IllegalArgumentException ? ex.getMessage() : "This server bookmark could not be saved.", Toast.LENGTH_LONG).show();
		}
	}

	/**
	 * Removes the selected saved server profile and refreshes the selector.
	 */
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

	/**
	 * adds a password row with its cipher selector and remove control
	 *
	 * @param valueOfThatKeyField typed cipher entry, legacy string, or {@code null} for an empty row
	 */
	private void addKeyField(Object valueOfThatKeyField)
	{
		LinearLayout keysContainer = findViewById(R.id.keys_container);

		View row = getLayoutInflater().inflate(R.layout.key_field_row, keysContainer, false);

		EditText keyInput = row.findViewById(R.id.key_input);
		keyInput.setSaveEnabled(false);
		keyInput.setTag(valueOfThatKeyField);
		keyInput.setText(MetadataKey.password(valueOfThatKeyField));
		Spinner cipher = row.findViewById(R.id.key_type);
		java.util.ArrayList<String> labels = new java.util.ArrayList<>(MetadataKey.LABELS);
		int selected = MetadataKey.TYPES.indexOf(MetadataKey.type(valueOfThatKeyField));
		if (selected < 0)
		{
			selected = labels.size();
			labels.add("Unsupported saved type");
		}
		ArrayAdapter<String> choices = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
		choices.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
		cipher.setAdapter(choices);
		cipher.setSelection(selected);
		cipher.setSaveEnabled(false);

		// servers without keys can leave this list empty
		row.findViewById(R.id.remove_key).setOnClickListener(v ->
		{
			keysContainer.removeView(row);
		});

		keysContainer.addView(row);
	}

	/**
	 * Loads the saved identity and offers recovery when legacy records conflict.
	 * Applies a saved mode or waits for permissions before offering a mode choice.
	 */
	private void ensureIdentityAndMode()
	{
		if (!this.settings.loadPersistedState())
		{
			if ("legacy_identity_conflict".equals(this.settings.getInitializationError()))
			{
				new AlertDialog.Builder(this)
					.setTitle("Choose the identity to keep")
					.setMessage("Two different saved identities were found. Neither has been replaced. Choose the identity used by your saved connection settings, or the older app identity. Keep your identity backup until you confirm your server permissions.")
					.setPositiveButton("Connection identity", (dialog, which) ->
					{
						this.finishIdentityRecovery(true);
					})
					.setNegativeButton("Older app identity", (dialog, which) ->
					{
						this.finishIdentityRecovery(false);
					})
					.setNeutralButton("Keep both for now", null)
					.show();
				return;
			}
			Toast.makeText(this, "Identity settings could not be loaded. Existing data was kept.", Toast.LENGTH_LONG).show();
			return;
		}

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

	/**
	 * Commits the chosen legacy identity and requests connection after successful recovery.
	 *
	 * @param useSettingsIdentity whether to retain the identity from the saved connection settings
	 */
	private void finishIdentityRecovery(boolean useSettingsIdentity)
	{
		if (!this.settings.recoverLegacyIdentity(useSettingsIdentity))
		{
			Toast.makeText(this, "Identity could not be saved. Existing data was kept.", Toast.LENGTH_LONG).show();
			return;
		}
		if (this.backgroundService != null && this.backgroundService.nodeBridge != null)
		{
			this.backgroundService.nodeBridge.sendConnect();
		}
	}

	/**
	 * Requests a single field update against the latest committed settings.
	 *
	 * @param fieldName settings field to update
	 * @param value new string value
	 */
	private void mergeFieldIntoStoredSettings(String fieldName, String value)
	{
		this.settings.updateSetting(fieldName, value);
	}

	/**
	 * Shows the initial mode choice after permissions finish, unless a choice or dialog already exists.
	 */
	public void promptForAppModeIfNeeded()
	{
		if (this.preferences == null || this.isAppModeDialogVisible || this.isFinishing())
		{
			return;
		}

		if (this.preferences.getString("app_mode", "").equals("") == false)
		{
			this.offerFirstConnection();
			return;
		}

		this.showFirstRunModeDialog();
	}

	/**
	 * Displays the guarded Simple or Advanced mode choice.
	 */
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

	/**
	 * Stores the selected mode and publishes it to the current page.
	 *
	 * @param mode selected application mode
	 */
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
		this.offerFirstConnection();
	}

	/**
	 * Reports native activity visibility to the Node owner, preserving idle after a declined call.
	 * While the file chooser of the page is open, the hidden report keeps the session out of idle:
	 * the chooser is another app's screen in front of this activity, not the user leaving the app.
	 *
	 * @param isVisible whether this activity is currently on screen
	 */
	private void reportUiVisibility(boolean isVisible)
	{
		if (this.backgroundService == null || this.backgroundService.nodeBridge == null)
		{
			return;
		}

		// open file chooser
		boolean isFileChooserOpen = !isVisible && this.backgroundService.fileChooserCallback != null;
		this.backgroundService.nodeBridge.sendUiVisible(isVisible, (isVisible && BackgroundService.suppressNextIdleExit) || isFileChooserOpen);
	}

	/**
	 * Restores the WebView and presence only after this activity has actually resumed.
	 */
	public void handleOnResumeOnCreate()
	{
		if (!this.resumed || this.destroyed)
		{
			this.reportUiVisibility(false);
			return;
		}
		//both entry points: resume, and service connect for when the activity beat the binding
		if (this.backgroundService != null)
		{
			this.backgroundService.resumePageMedia(this);
		}
		this.reportUiVisibility(true);
		if (this.nativeSettingsOpen)
		{
			return;
		}

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

						// set window owner
						this.backgroundService.setWebViewWindowOwner(this);
						frameLayout.addView(this.backgroundService.webView, params);
					}
				}
				else
				{
					//the app was started and there is no attached window, probably first start
					Log.d("Info", "[lemonchat] app started for first time");
					FrameLayout frameLayout = this.findViewById(R.id.root_container);

					// set window owner
					this.backgroundService.setWebViewWindowOwner(this);

					if (!(this.backgroundService.webView.getParent() instanceof ViewGroup))
					{
						frameLayout.addView(this.backgroundService.webView);
					}
				}

				//node may have (re)connected while the page's retry timers were throttled in the
				//background; poke it so reattaching starts now instead of waiting out a countdown
				this.backgroundService.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__request_loopback_reattach === 'function')\n{\n    JavascriptJavaBridge__request_loopback_reattach();\n}", null);

				//re-deliver node's current phase: a push lost while the page was busy or detached
				//left the status frozen (the stuck "generating the identity key" episode)
				if (this.backgroundService.nodeBridge != null)
				{
					this.backgroundService.nodeBridge.pushConnectionPhaseToWebview();
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
					this.backgroundService.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_come_from_idle_mode_request === 'function')\n{\n    JavascriptJavaBridge__send_come_from_idle_mode_request();\n}", null);
				}
			}
		}
	}

	/**
	 * Releases foreground ownership and asks the detached page to stop idle sensitive media work.
	 */
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
			if (MainActivity.instance == null && this.backgroundService != null && this.backgroundService.webView != null
				&& this.backgroundService.fileChooserCallback == null)
			{
				this.backgroundService.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_go_to_idle_mode_request === 'function')\n{\n    JavascriptJavaBridge__send_go_to_idle_mode_request();\n}", null);
			}

		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onCreate" + ex.getMessage());
			ex.printStackTrace();
		}
		finally
		{
			super.onStop();
		}
	}

	/**
	 * Cancels activity native callbacks and releases its service binding even during early teardown.
	 */
	@Override protected void onDestroy()
	{
		this.destroyed = true;
		if (this.permissions != null)
		{
			this.permissions.destroy();
		}
		try
		{
			Log.d("Info", "[lemonchat] onDestroy");

			//the window is closing, so a file picker it opened will never answer. cleared before
			//the line below, which refuses to report anything while a picker is remembered
			if (this.backgroundService != null)
			{
				this.backgroundService.abandonPendingFileChooser(this);
				this.backgroundService.cancelPendingPermission(this);
			}

			this.handleOnStopOnDestroyOnPause();
			if (MainActivity.instance == null && this.backgroundService != null && this.backgroundService.webView != null)
			{
				this.backgroundService.webView.evaluateJavascript("if (typeof JavascriptJavaBridge__send_go_to_idle_mode_request === 'function')\n{\n    JavascriptJavaBridge__send_go_to_idle_mode_request();\n}", null);
			}

		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onDestroy" + ex.getMessage());
			ex.printStackTrace();
		}
		finally
		{
			try
			{
				if (this.serviceBound)
				{
					this.unbindService(this.serviceConnection);
					this.serviceBound = false;
				}
			}
			finally
			{
				super.onDestroy();
			}
		}
	}

	/**
	 * Populates the native connection form from the latest settings snapshot using defaults for missing fields.
	 *
	 * @return whether form loading completed without an exception
	 */
	public boolean LoadSettings()
	{
		boolean result = false;

		try
		{
			String json = this.settings.getJsonSettings();

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
				boolean isAutoconnectEnabled = settingsJson.optBoolean("is_autoconnect_enabled", true);

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
						Object value = keys.opt(i);
						if (value != null)
						{
							this.addKeyField(value);
						}
					}
				}

				//the saved list can have grown since the screen was last built
				this.refreshServerBookmarkSpinner();

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

	/**
	 * Closes the native settings panel and attaches the chat view to the foreground container.
	 */
	private void restoreWebViewToForeground()
	{
		this.nativeSettingsOpen = false;
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

			// set window owner
			this.backgroundService.setWebViewWindowOwner(this);
			frameLayout.addView(this.backgroundService.webView, params);
		}
	}

	/**
	 * Closes the native settings panel without saving its current form values.
	 *
	 * @param view button that triggered the action
	 */
	public void closeSettings(View view)
	{
		this.restoreWebViewToForeground();
	}

	/**
	 * Commits the native connection form and returns to chat after a successful save.
	 * Saving settings does not create a new Connect intent.
	 *
	 * @param view button that triggered the action
	 */
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

		boolean isAutoconnectEnabled = autoConnect.isChecked();

		// Create the master JSONObject
		JSONObject settingsJson = new JSONObject();

		try
		{
			// Put static values
			settingsJson.put("host", ip);
			settingsJson.put("port", portStr);
			settingsJson.put("default_username", defaultUsernameString);

			settingsJson.put("is_autoconnect_enabled", isAutoconnectEnabled);



			//Only the selected mode is patched here; the canonical identity stays with ChatSettings.

			settingsJson.put("app_mode", this.preferences.getString("app_mode", "advanced"));

			// Create a JSONArray for the dynamic keys. servers may run with no metadata keys at all,
			// so an empty row is simply skipped instead of failing the whole save - it just means
			// "no key here". a server that does use keys rejects the connection on its own.
			settingsJson.put("metadata_keys", this.readKeyFields());
			if (!this.settings.saveJsonSettings(settingsJson.toString()))
			{
				Toast.makeText(this, "Settings could not be saved. Please try again.", Toast.LENGTH_LONG).show();
				return;
			}

			// pushing to node and the webview is the save hook's job (ChatSettings.onSettingsSaved)
		}
		catch (Exception e)
		{
			Log.e("lemonchat", "saveSettings failed", e);
			Toast.makeText(this, e instanceof IllegalArgumentException ? e.getMessage() : "Settings could not be saved. Please try again.", Toast.LENGTH_LONG).show();
			return;
		}

		this.restoreWebViewToForeground();
	}
}
